// MbedArduinoJson - https://arduinojson.org  -  https://github.com/ATM-HSW/libMbedArduinoJson.git
// Copyright Benoit Blanchon 2014-2021, Dr.Olaf Hagendorf 2021
// MIT License
//
// This example shows how to generate a JSON document with libMbedArduinoJson.
//
// https://arduinojson.org/v6/example/generator/

#include "mbed.h"
#include "ArduinoJson.h"

int main() {
  // Allocate the JSON document
  //
  // Inside the brackets, 200 is the RAM allocated to this document.
  // Don't forget to change this value to match your requirement.
  // Use https://arduinojson.org/v6/assistant to compute the capacity.
  //StaticJsonDocument<200> doc;

  // StaticJsonObject allocates memory on the stack, it can be
  // replaced by DynamicJsonDocument which allocates in the heap.
  DynamicJsonDocument doc(200);

  // Add values in the document
  doc["sensor"] = "gps";
  doc["time"] = 1351824120;

  // Add an array.
  JsonArray data = doc.createNestedArray("data");
  data.add(48.756080);
  data.add(2.302038);

  // Generate the minified JSON and send it to the Serial port.
  string output;
  serializeJson(doc, output);
  printf("%s\n", output.c_str());
  // The above line prints:
  // {"sensor":"gps","time":1351824120,"data":[48.756080,2.302038]}


  // Generate the prettified JSON and send it to the Serial port.
  serializeJsonPretty(doc, output);
  printf("%s\n", output.c_str());
  // The above line prints:
  // {
  //   "sensor": "gps",
  //   "time": 1351824120,
  //   "data": [
  //     48.756080,
  //     2.302038
  //   ]
  // }

  while(true) thread_sleep_for(1000);
}

// See also
// --------
//
// https://arduinojson.org/ contains the documentation for all the functions
// used above. It also includes an FAQ that will help you solve any
// serialization problem.
//
// The book "Mastering ArduinoJson" contains a tutorial on serialization.
// It begins with a simple example, like the one above, and then adds more
// features like serializing directly to a file or an HTTP request.
// Learn more at https://arduinojson.org/book/
// Use the coupon code TWENTY for a 20% discount ❤❤❤❤❤
