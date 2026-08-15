cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build
ninja -C build  
.\build\src\exchange.exe
