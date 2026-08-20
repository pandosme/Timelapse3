/* Only linked into the AddressSanitizer build (see Dockerfile.asan). ASan calls this
   before main to pick up its configuration, which is the only way to configure it here:
   the ACAP has no way to set environment variables for its own service.

   Findings go to stderr, which systemd already routes into the device syslog - that is
   how "corrupted size vs. prev_size" from glibc reached the log in the first place. */
const char* __asan_default_options(void);

const char* __asan_default_options(void) {
    return "detect_leaks=0"          /* the app frees nothing at exit by design; leaks are noise here */
           ":abort_on_error=0"       /* report and keep going, so one finding does not hide the next */
           ":print_legend=0"
           ":symbolize=0"            /* no llvm-symbolizer on the camera; addr2line offline instead */
           ":log_to_syslog=1";
}
