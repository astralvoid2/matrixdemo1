/* Console entry point on Windows and Linux. The Windows GUI executable has
 * only wWinMain, so its linker cannot accidentally choose a console startup. */
int demo_main(int argc, char **argv);
int main(int argc, char **argv) { return demo_main(argc, argv); }
