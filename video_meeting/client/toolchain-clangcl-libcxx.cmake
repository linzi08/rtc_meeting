set(CMAKE_SYSTEM_NAME Windows)

set(LLVM_ROOT "D:/webrtc/src/third_party/llvm-build/Release+Asserts")
set(MSVC_ROOT "d:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/MSVC/14.44.35207")
set(WINSDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
set(WINSDK_VER "10.0.26100.0")

set(CMAKE_C_COMPILER   "${LLVM_ROOT}/bin/clang-cl.exe")
set(CMAKE_CXX_COMPILER "${LLVM_ROOT}/bin/clang-cl.exe")
set(CMAKE_LINKER       "${LLVM_ROOT}/bin/lld-link.exe")
set(CMAKE_AR           "${LLVM_ROOT}/bin/llvm-lib.exe")
set(CMAKE_RC_COMPILER  "${WINSDK_ROOT}/bin/${WINSDK_VER}/x64/rc.exe")
set(CMAKE_MT           "${WINSDK_ROOT}/bin/${WINSDK_VER}/x64/mt.exe")

set(MSVC_INC          "${MSVC_ROOT}/include")
set(WINSDK_UM_INC     "${WINSDK_ROOT}/Include/${WINSDK_VER}/um")
set(WINSDK_UCRT_INC   "${WINSDK_ROOT}/Include/${WINSDK_VER}/ucrt")
set(WINSDK_SHARED     "${WINSDK_ROOT}/Include/${WINSDK_VER}/shared")

set(MSVC_LIB_X64      "${MSVC_ROOT}/lib/x64")
set(WINSDK_UM_LIB     "${WINSDK_ROOT}/Lib/${WINSDK_VER}/um/x64")
set(WINSDK_UCRT_LIB   "${WINSDK_ROOT}/Lib/${WINSDK_VER}/ucrt/x64")

set(CMAKE_C_FLAGS   "")
set(CMAKE_CXX_FLAGS "")

string(CONCAT CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
  "${MSVC_INC};${WINSDK_UM_INC};${WINSDK_UCRT_INC};${WINSDK_SHARED}")

string(CONCAT CMAKE_C_STANDARD_INCLUDE_DIRECTORIES
  "${MSVC_INC};${WINSDK_UM_INC};${WINSDK_UCRT_INC};${WINSDK_SHARED}")

string(CONCAT CMAKE_EXE_LINKER_FLAGS_INIT
  " -libpath:\"${MSVC_LIB_X64}\""
  " -libpath:\"${WINSDK_UM_LIB}\""
  " -libpath:\"${WINSDK_UCRT_LIB}\"")

string(CONCAT CMAKE_SHARED_LINKER_FLAGS_INIT
  " -libpath:\"${MSVC_LIB_X64}\""
  " -libpath:\"${WINSDK_UM_LIB}\""
  " -libpath:\"${WINSDK_UCRT_LIB}\"")

string(CONCAT CMAKE_STATIC_LINKER_FLAGS_INIT "")