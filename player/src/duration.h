#pragma once

#include <QtGlobal>
#include <QString>

// Container-header duration readers. Deliberately not libmpv: a second core
// doing hundreds of loadfile() calls leaves the process deadlocking in an
// injected IME's detach handler at exit (measured: 0/8 crashes at 20 probes,
// 5/8 at 500). The header is all a duration needs, so this just reads bytes.
//
// Returns seconds, or 0 when the container is not one of the supported ones or
// the header does not carry a credible length.
double readDuration(const QString &path);
