package main

import "github.com/nominal-io/nominal-simulink/nmproxy/frame"

// frameVersion is reported by -version so an operator can check that a host and this
// proxy agree. Worth surfacing because a mismatch has no other symptom: there is no
// back-channel, so a host speaking a version this proxy rejects looks exactly like a
// host that is not sending at all.
const frameVersion = frame.Version
