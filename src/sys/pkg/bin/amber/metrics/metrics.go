package metrics

import (
	"time"
)

type metricID uint32

const (
	_ metricID = iota
	metricSystemUpToDate
	metricOtaStart
	metricOtaResultAttempt
	metricOtaResultDuration
	metricOtaResultFreeSpaceDelta
)

type Status uint32

const (
	// The names and integer values must correspond to the event_codes in the
	// |status_codes| dimension of the metrics in our Cobalt registry at
	// https://cobalt-analytics.googlesource.com/config/+/refs/heads/master/fuchsia/software_delivery/config.yaml
	StatusSuccess Status = iota
	StatusFailure
	StatusFailureStorage
	StatusFailureStorageOutOfSpace
	StatusFailureNetworking
	StatusFailureUntrustedTufRepo
)

func StatusFromError(err error) Status {
	if err == nil {
		return StatusSuccess
	}

	// TODO: Fix when rewriting in rust. The current system_updater implementation
	// forces us into comparing strings (the other option would be to significantly
	// change the system updater code, which is impractical since it's being rewritten anyway)
	if err.Error() == "failed getting packages: fetch: Resolve status: zx.Status(-71)" {
		return StatusFailureUntrustedTufRepo
	}
	return StatusFailure
}

// Log synchronously submits the given metric to cobalt
func Log(metric Metric) {
	metric.log()
}

// Metric is a cobalt metric that can be submitted to cobalt
type Metric interface {
	log()
}

type SystemUpToDate struct {
	Initiator Initiator
	Version   string
	When      time.Time
}

func (m SystemUpToDate) log() {
	logEventMulti(metricSystemUpToDate, []uint32{uint32(m.Initiator), uint32(m.When.Local().Hour())}, "")
}

type OtaStart struct {
	Initiator Initiator
	Target    string
	When      time.Time
}

func (m OtaStart) log() {
	logEventMulti(metricOtaStart, []uint32{uint32(m.Initiator), uint32(m.When.Local().Hour())}, m.Target)
}

type OtaResultAttempt struct {
	Initiator Initiator
	Target    string
	Attempt   int64
	Phase     Phase
	Status    Status
}

func (m OtaResultAttempt) log() {
	logEventCountMulti(metricOtaResultAttempt, 0, m.Attempt,
		[]uint32{uint32(m.Initiator), uint32(m.Phase), uint32(m.Status)},
		m.Target)
}

type OtaResultDuration struct {
	Initiator Initiator
	Target    string
	Duration  time.Duration
	Phase     Phase
	Status    Status
}

func (m OtaResultDuration) log() {
	logElapsedTimeMulti(metricOtaResultDuration, m.Duration,
		[]uint32{uint32(m.Initiator), uint32(m.Phase), uint32(m.Status)},
		m.Target)
}

type OtaResultFreeSpaceDelta struct {
	Initiator      Initiator
	Target         string
	FreeSpaceDelta int64
	Duration       time.Duration
	Phase          Phase
	Status         Status
}

func (m OtaResultFreeSpaceDelta) log() {
	logEventCountMulti(metricOtaResultFreeSpaceDelta, 0, m.FreeSpaceDelta,
		[]uint32{uint32(m.Initiator), uint32(m.Phase), uint32(m.Status)},
		m.Target)
}
