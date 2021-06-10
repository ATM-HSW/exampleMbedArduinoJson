// ArduinoJson - https://arduinojson.org  -  https://github.com/ATM-HSW/libMbedArduinoJson.git
// Copyright Benoit Blanchon 2014-2021, Dr.Olaf Hagendorf 2021
// MIT License
//
// This example shows the different ways you can use String with libMbedArduinoJson.
//
// Use String objects sparingly, because libMbedArduinoJson duplicates them in the
// JsonDocument. Prefer plain old char[], as they are more efficient in term of
// code size, speed, and memory usage.
//
// https://arduinojson.org/v6/example/string/

#include "mbed.h"
#include "ArduinoJson.h"

int main() {
  DynamicJsonDocument doc(1024);

  // You can use a String as your JSON input.
  // WARNING: the string in the input  will be duplicated in the JsonDocument.
  string input = "{\"sensor\":\"gps\",\"time\":1351824120,\"data\":[48.756080,2.302038]}";
  deserializeJson(doc, input);
  JsonObject obj = doc.as<JsonObject>();

  // You can use a String to get an element of a JsonObject
  // No duplication is done.
  long time = obj[string("time")];

  // You can use a String to set an element of a JsonObject
  // WARNING: the content of the String will be duplicated in the JsonDocument.
  obj[string("time")] = time;

  // You can get a String from a JsonObject or JsonArray:
  // No duplication is done, at least not in the JsonDocument.
  string sensor = obj["sensor"];

  // Unfortunately, the following doesn't work (issue #118):
  // sensor = obj["sensor"]; // <-  error "ambiguous overload for 'operator='"
  // As a workaround, you need to replace by:
  sensor = obj["sensor"].as<string>();

  // You can set a String to a JsonObject or JsonArray:
  // WARNING: the content of the String will be duplicated in the JsonDocument.
  obj["sensor"] = sensor;

  // It works with serialized() too:
  obj["sensor"] = serialized(sensor);

  // You can also concatenate strings
  // WARNING: the content of the String will be duplicated in the JsonDocument.
  obj[string("sen") + "sor"] = string("gp") + "s";

  // You can compare the content of a JsonObject with a String
  if (obj["sensor"] == sensor) {
    // ...
  }

  // Lastly, you can print the resulting JSON to a String
  string output;
  serializeJson(doc, output);

  printf("%s\n", output.c_str());

  while(true) thread_sleep_for(1000);
}


// See also
// --------
//
// https://arduinojson.org/ contains the documentation for all the functions
// used above. It also includes an FAQ that will help you solve any problem.
//
// The book "Mastering ArduinoJson" contains a quick C++ course that explains
// how your microcontroller stores strings in memory. On several occasions, it
// shows how you can avoid String in your program.
// Learn more at https://arduinojson.org/book/
// Use the coupon code TWENTY for a 20% discount ❤❤❤❤❤
