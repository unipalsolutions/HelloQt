# HelloQt

تطبيق سطح مكتب بسيط مبني بـ Qt 6 (Qt Widgets) و C++17. يفتح نافذة واحدة تعرض كلمة "مرحبا".

## تنزيل نسخة جاهزة للتشغيل

يبني GitHub Actions التطبيق تلقائياً على Windows عند كل push (الملف `.github/workflows/build-windows.yml`):

1. افتح تبويب **Actions** في المستودع، ثم اختر آخر تشغيل ناجح لـ **Build Windows**.
2. نزّل الملف `HelloQt-windows-x64` من قسم **Artifacts**.
3. فك الضغط، ثم شغّل `HelloQt.exe`. لا يحتاج التطبيق إلى تثبيت Qt.

## المتطلبات للبناء محلياً

- Qt 6 مع مكوّن Qt Widgets، بنسخة تطابق المترجم (MSVC 2022 64-bit أو MinGW 64-bit)
- CMake 3.16 أو أحدث
- مترجم C++17: Visual Studio 2022 أو MinGW المرفق مع Qt

## البناء

### باستخدام MSVC

افتح **x64 Native Tools Command Prompt for VS 2022** ثم نفّذ:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build
```

### باستخدام MinGW

افتح **Qt 6.8.3 (MinGW 13.1.0 64-bit)** من قائمة Start ثم نفّذ:

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\mingw_64
cmake --build build
```

> عدّل المسار `C:\Qt\6.8.3\...` حسب مكان تثبيت Qt على جهازك.

## التشغيل

```bat
build\HelloQt.exe
```

عند التشغيل من خارج طرفية Qt، يجب أن يكون مجلد `bin` الخاص بـ Qt في `PATH`، أو جهّز نسخة التوزيع كما في القسم التالي.

## تجهيز نسخة قابلة للتوزيع (windeployqt)

```bat
mkdir dist\HelloQt
copy build\HelloQt.exe dist\HelloQt\
C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations dist\HelloQt\HelloQt.exe
```

ينسخ `windeployqt` مكتبات Qt والإضافات اللازمة (مثل `platforms\qwindows.dll`) بجانب الملف التنفيذي. بعدها يمكنك ضغط المجلد `dist\HelloQt` وتوزيعه.

- مع MinGW: استخدم `windeployqt.exe` من مجلد `mingw_64\bin`، وهو ينسخ مكتبات MinGW وقت التشغيل تلقائياً.
- مع MSVC: انسخ مكتبات Visual C++ وقت التشغيل أيضاً (`vcruntime140.dll` و `msvcp140.dll` وغيرهما) من المجلد `%VCToolsRedistDir%x64\Microsoft.VC143.CRT`، أو اطلب من المستخدم تثبيت Visual C++ Redistributable.

## هيكل المشروع

```
HelloQt/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── mainwindow.h
│   └── mainwindow.cpp
├── .gitignore
└── README.md
```
