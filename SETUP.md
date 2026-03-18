# Quick Setup (New Machine)

```bash
pip install aqtinstall

python -m aqt install-qt windows desktop 6.8.2 win64_mingw --outputdir C:\Qt -m qtwebsockets
python -m aqt install-tool windows desktop tools_mingw1310 --outputdir C:\Qt
python -m aqt install-tool windows desktop tools_cmake --outputdir C:\Qt
python -m aqt install-tool windows desktop tools_ninja --outputdir C:\Qt

git clone https://gitlab.123net.link/kalin/talk-desktop-qt.git C:\Projects\talk-desktop-qt
cd C:\Projects\talk-desktop-qt

set PATH=C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;%PATH%
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build build

set PATH=C:\Qt\6.8.2\mingw_64\bin;%PATH%
.\build\talk-qt.exe
```
