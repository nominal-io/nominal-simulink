package main

import "github.com/nominal-io/nominal-simulink/nmproxy/frame"

// frameVersion is reported by -version so an operator can check that a host and this
// proxy agree. The host gets no reply, so a mismatch shows only on the proxy, as
// wrong_version in the stats and a warning in the log.
const frameVersion = frame.Version

// version is the release this binary was built from, stamped at build time with
// -ldflags "-X main.version=<version>". A plain `go build` leaves it as "dev".
var version = "dev"
