// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const TemplateHtml = `
<html>
<body bgcolor="black">
<font color="white">

Signature: {{ .Signature }}<br />
<br />
UNUSED LICENSES:<br />
<br />
{{ range $_, $license := .Unused }}
================================================================================<br />
ORIGIN: <ORIGIN><br />
TYPE: <TYPE><br />
--------------------------------------------------------------------------------<br />
<LICENSE><br />
{{ (getPattern $license) }}<br />
================================================================================<br />
}<br />
{{ end }}
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~<br />
<br />
USED LICENSES:<br />
<br />
{{ range $_, $license := .Used }}
================================================================================<br />
LIBRARY: <LIBRARY><br />
ORIGIN: <ORIGIN><br />
TYPE: <TYPE><br />
{{ range $file := (getFiles $license) }}FILE: {{ $file }}<br />
{{ end }}
<br />
--------------------------------------------------------------------------------<br />
<LICENSE><br />
{{ (getPattern $license) }}<br />
================================================================================<br />
<br />
{{ end }}

</font>
</body>
</html>
`
