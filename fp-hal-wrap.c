/* Exec real fps_hal with LD_PRELOAD of the in-process QSEE hook. */
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	(void)argc;
	setenv("LD_PRELOAD",
	       "/vendor/lib64/android.hardware.automotive.vehicle@2.0-manager-lib.so",
	       1);
	execv("/data/local/tmp/fp-hal.real", argv);
	return 127;
}
