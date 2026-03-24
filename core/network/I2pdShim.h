// AeonBrowser — I2pdShim.h
#pragma once

namespace I2pdShim {
    // Launch i2pd.exe as background child process.
    // app_install_dir = path to Aeon Browser install dir (e.g. "C:\Program Files\Aeon")
    bool Start(const char* app_install_dir);

    // Stop the i2pd child process.
    void Stop();

    // Returns true if i2pd process is alive.
    bool IsRunning();
}
