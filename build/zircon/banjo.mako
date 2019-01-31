<%include file="header.mako" />

import("//build/banjo/banjo.gni")

banjo("${data.name}") {
  name = "${data.library}"

  sdk_category = "partner"

  sources = [
    % for source in sorted(data.sources):
    "${source}",
    % endfor
  ]

  deps = [
    % for dep in sorted(data.banjo_deps):
    "../${dep}",
    % endfor
  ]
}
