# Compiling and Using

```
# git clone https://github.com/CurlyMoo/HeishaDaemon.git
# cd HeishaDaemon
# mkdir build
# cd build
# cmake ..
# make -j4
# ./start
```

Place your rules in the repository root folder called `rules.txt`.

Edit the `main.cpp` source file to match the IP or your broker and the MQTT topics of your HeishaMon.

Currently an external temperature MQTT topic is used to override the default HeishaMon `Room_Thermostat_Temp` value.

Additional debugging information is shown when started so you can see what the underlying variable values are.

The `rules.txt` contains a WIP ruleset i'm currently running.
