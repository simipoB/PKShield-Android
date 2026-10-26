// The python executable for the app: CPython for Android is shipped as a library (libpython3.14.so); this is its main
#include <Python.h>
int main(int argc, char **argv) { return Py_BytesMain(argc, argv); }
