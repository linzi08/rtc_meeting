mkdir build
cd build

cmake -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="D:/webrtc/src/buildtools/win/ninja.exe" ^
  -DCMAKE_C_COMPILER="D:/webrtc/src/third_party/llvm-build/Release+Asserts/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="D:/webrtc/src/third_party/llvm-build/Release+Asserts/bin/clang-cl.exe" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DLIBWEBRTC_INCLUDE_PATH="D:/webrtc/src" ^
  -DLIBWEBRTC_BINARY_PATH="D:/webrtc/src/out/Default/obj" ^
  -DUSE_SYSTEM_JSON=ON ^
  -DJSON_INCLUDE_DIR="D:/work/work_code/libmediasoupclient/third_party" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded" ^
  ..

cmake --build .