# this file is shared for all targets
# and it is actually located in the root folder of the project
isEmpty(PREFIX) {
    PREFIX = /usr/local
}
BINDIR = $$PREFIX/bin
DATADIR = $$PREFIX/share
isEmpty(LIBEXECDIR) {
    LIBEXECDIR = $$PREFIX/libexec/mediawriter
}
linux {
    DEFINES += BINDIR=\\\"$$BINDIR\\\"
    DEFINES += DATADIR=\\\"$$DATADIR\\\"
    DEFINES += LIBEXECDIR=\\\"$$LIBEXECDIR\\\"
}
QMAKE_LIBDIR += $$top_builddir/lib
INCLUDEPATH += $$top_srcdir/lib/
