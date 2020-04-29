// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package system_updater

import (
	"context"
	"flag"
	"fmt"
	"strconv"
	"syscall/zx"
	"syscall/zx/fdio"
	"syscall/zx/fidl"
	"time"

	appcontext "app/context"
	devmgr "fidl/fuchsia/device/manager"
	"fidl/fuchsia/space"
	"syslog"

	"metrics"
)

var (
	initiator     metrics.Initiator
	startTime     time.Time
	sourceVersion string
	targetVersion string
	updateURL     string
	reboot        bool
	skipRecovery  bool
)

func run(ctx *appcontext.Context) (err error) {
	syslog.Infof("starting system update at %s", startTime)
	syslog.Infof("initiator: %s", initiator)
	if sourceVersion != "" {
		syslog.Infof("source version: %s", sourceVersion)
	}
	if targetVersion != "" {
		syslog.Infof("target version: %s", targetVersion)
	}
	if updateURL != "" {
		syslog.Infof("update URL: %s", updateURL)
	}
	syslog.Infof("reboot after update: %t", reboot)

	metrics.Log(metrics.OtaStart{
		Initiator: initiator,
		Target:    targetVersion,
		When:      startTime,
	})
	phase := metrics.PhaseEndToEnd

	GcPackages(ctx)

	var queryFreeSpace func() int64

	if blobfs, err := OpenBlobfs(); err == nil {
		defer blobfs.Close(context.Background())
		queryFreeSpace = func() int64 {
			n, err := blobfs.QueryFreeSpace()
			if err != nil {
				syslog.Errorf("error querying blobfs free space! %s", err)
				return -1
			}
			return n
		}
	} else {
		syslog.Errorf("error opening blobfs! %s", err)
		queryFreeSpace = func() int64 {
			return -1
		}
	}
	freeSpaceStart := queryFreeSpace()

	history := IncrementOrCreateUpdateHistory(sourceVersion, targetVersion, startTime)
	defer func() {
		if err != nil {
			metrics.Log(metrics.OtaResultAttempt{
				Initiator: initiator,
				Target:    targetVersion,
				Attempt:   int64(history.Attempts),
				Phase:     phase,
				Status:    metrics.StatusFromError(err),
			})
			metrics.Log(metrics.OtaResultDuration{
				Initiator: initiator,
				Target:    targetVersion,
				Duration:  time.Since(startTime),
				Phase:     phase,
				Status:    metrics.StatusFromError(err),
			})
			metrics.Log(metrics.OtaResultFreeSpaceDelta{
				Initiator:      initiator,
				Target:         targetVersion,
				FreeSpaceDelta: queryFreeSpace() - freeSpaceStart,
				Duration:       time.Since(startTime),
				Phase:          phase,
				Status:         metrics.StatusFromError(err),
			})
			if err := history.Save(); err != nil {
				syslog.Errorf("error writing update history: %s", err)
			}
		}
	}()

	resolver, err := ConnectToPackageResolver(ctx)
	if err != nil {
		return fmt.Errorf("unable to connect to update service: %s", err)
	}
	defer resolver.Close()

	dataSink, bootManager, err := ConnectToPaver(ctx)
	if err != nil {
		return fmt.Errorf("unable to connect to paver service: %s", err)
	}
	defer dataSink.Close()
	defer bootManager.Close()

	updatePkg, err := CacheUpdatePackage(updateURL, resolver)
	if err != nil {
		return fmt.Errorf("error caching update package! %s", err)
	}
	defer updatePkg.Close()

	pkgs, imgs, err := ParseRequirements(updatePkg)
	if err != nil {
		return fmt.Errorf("could not parse requirements: %s", err)
	}

	if err := ValidateUpdatePackage(updatePkg); err != nil {
		return fmt.Errorf("failed to validate update package: %s", err)
	}

	updateMode, err := ParseUpdateMode(updatePkg)
	if err != nil {
		return fmt.Errorf("failed to parse update mode: %s", err)
	}

	phase = metrics.PhasePackageDownload
	if updateMode == UpdateModeNormal {
		if err := FetchPackages(pkgs, resolver); err != nil {
			return fmt.Errorf("failed getting packages: %s", err)
		}
	}

	if err := ValidateImgs(imgs, updatePkg, updateMode); err != nil {
		return fmt.Errorf("failed to validate imgs: %s", err)
	}

	phase = metrics.PhaseImageWrite
	if err := WriteImgs(dataSink, bootManager, imgs, updatePkg, updateMode, skipRecovery); err != nil {
		return fmt.Errorf("error writing image file: %s", err)
	}

	phase = metrics.PhaseSuccessPendingReboot
	metrics.Log(metrics.OtaResultAttempt{
		Initiator: initiator,
		Target:    targetVersion,
		Attempt:   int64(history.Attempts),
		Phase:     phase,
		Status:    metrics.StatusFromError(nil),
	})
	metrics.Log(metrics.OtaResultDuration{
		Initiator: initiator,
		Target:    targetVersion,
		Duration:  time.Since(startTime),
		Phase:     phase,
		Status:    metrics.StatusFromError(nil),
	})
	metrics.Log(metrics.OtaResultFreeSpaceDelta{
		Initiator:      initiator,
		Target:         targetVersion,
		FreeSpaceDelta: queryFreeSpace() - freeSpaceStart,
		Duration:       time.Since(startTime),
		Phase:          phase,
		Status:         metrics.StatusFromError(nil),
	})

	if err := UpdateCurrentChannel(); err != nil {
		syslog.Errorf("%v", err)
	}

	if err := history.Save(); err != nil {
		syslog.Errorf("error writing update history: %s", err)
	}

	if reboot || updateMode == UpdateModeForceRecovery {
		syslog.Infof("system update complete, rebooting...")
		SendReboot()
	} else {
		syslog.Infof("system update complete, new version will run on next boot.")
	}

	return nil
}

func SendReboot() {
	channel_local, channel_remote, err := zx.NewChannel(0)
	if err != nil {
		syslog.Errorf("error creating channel: %s", err)
		return
	}

	err = fdio.ServiceConnect(
		"/svc/fuchsia.device.manager.Administrator", zx.Handle(channel_remote))
	if err != nil {
		syslog.Errorf("error connecting to devmgr service: %s", err)
		return
	}

	var administrator = devmgr.AdministratorWithCtxInterface(
		fidl.ChannelProxy{Channel: zx.Channel(channel_local)})
	var status int32
	status, err = administrator.Suspend(context.Background(), devmgr.SuspendFlagReboot)
	if err != nil || status != 0 {
		syslog.Errorf("error sending restart to Administrator: %s status: %d", err, status)
	}
}

func GcPackages(ctx *appcontext.Context) {
	req, pxy, err := space.NewManagerWithCtxInterfaceRequest()
	if err != nil {
		syslog.Errorf("Error creating space Manager request: %s", err)
		return
	}
	ctx.ConnectToEnvService(req)
	res, err := pxy.Gc(context.Background())
	if err != nil {
		syslog.Errorf("Error collecting garbage: %s", err)
	}
	if res.Which() == space.ManagerGcResultErr {
		syslog.Errorf("Error collecting garbage: %s", res.Err)
	}
	pxy.Close()
}

type InitiatorValue struct {
	initiator *metrics.Initiator
}

func (i InitiatorValue) String() string {
	if i.initiator == nil {
		return ""
	}
	return i.initiator.String()
}

func (i InitiatorValue) Set(s string) error {
	return i.initiator.Parse(s)
}

func Main() {
	ctx := appcontext.CreateFromStartupInfo()

	{
		if l, err := syslog.NewLoggerWithDefaults(ctx.Connector(), "system-updater"); err != nil {
			fmt.Println(err)
		} else {
			syslog.SetDefaultLogger(l)
		}
	}

	metrics.Register(ctx)

	// Can't use a bool var since not allowed in the argument scheme we want (--foo bar).
	var rebootString, skipRecoveryString string

	flag.Var(&InitiatorValue{&initiator}, "initiator", "what started this update: manual or automatic")
	start := flag.Int64("start", time.Now().UnixNano(), "start time of update attempt, as unix nanosecond timestamp")
	flag.StringVar(&sourceVersion, "source", "", "current OS version")
	flag.StringVar(&targetVersion, "target", "", "target OS version")
	flag.StringVar(&updateURL, "update", "fuchsia-pkg://fuchsia.com/update", "update package URL")
	flag.StringVar(&rebootString, "reboot", "true", "if true, reboot the system after successful OTA")
	flag.StringVar(&skipRecoveryString, "skip-recovery", "false", "if true, don't update the recovery image")
	flag.Parse()

	if len(flag.Args()) != 0 {
		syslog.Fatalf("unexpected arguments: %s", flag.Args())
	}

	startTime = time.Unix(0, *start)

	// Verify the bool string is actually a bool.
	var e error
	reboot, e = strconv.ParseBool(rebootString)
	if e != nil {
		fmt.Printf("Unable to parse reboot string: [%v]. Assuming reboot=true", e)
		reboot = true
	}

	skipRecovery, e = strconv.ParseBool(skipRecoveryString)
	if e != nil {
		fmt.Printf("Unable to parse skip-recovery string: [%v]. Assuming skip-recovery=false", e)
		skipRecovery = false
	}

	if err := run(ctx); err != nil {
		syslog.Fatalf("%s", err)
	}
}
