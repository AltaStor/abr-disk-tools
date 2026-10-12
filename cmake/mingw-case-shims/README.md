# MinGW header case shims

The AWS SDK's Windows sources spell system headers the way the Windows SDK
names them on disk — `<Windows.h>`, `<WinSock2.h>`, `<Userenv.h>` — and
Windows file systems do not care. MinGW-w64 ships the same headers in lower
case, so on a Linux build host, whose file system does care, those includes
find nothing.

Each file here is one of those capitalized names, and does nothing but include
the lower-case header it stands for. The overlay triplet in `../triplets` adds
this directory to the include path of the `aws-sdk-cpp` port, and only when
the build host is not Windows: on a case-insensitive file system a shim would
find itself instead of the header it forwards to.

Add a name here when a new SDK version starts including another one.
