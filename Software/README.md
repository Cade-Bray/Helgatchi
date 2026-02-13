# Building and deploying (old code)
You will need
* VSCode
  * PlatformIO VSCode extension
* ESP32S3
* SPI connected screen

1. Clone the repository
2. Navigate into the `Helgatchi/Software/Helgatchi Core` directory
3. Initialize Platform.IO project (PIO -> Open Project -> Navigate to Helgatchi Core folder)
4. Build the code, will probably take a while to set up (Project tasts -> Build)
5. Build the filesystem (Project tasks -> Build Filesystem Image)
6. Plug in your ESP
7. Erase Flash
8. Upload Filesystem Image
9. Upload and Monitor

This should flash the ESP with filesystem and the code, and the serial monitor should output what the device is doing.

# Building and deploying (LVGL)
TODO
