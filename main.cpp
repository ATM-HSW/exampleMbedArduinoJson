/*
 * Copyright (c) 2006-2020 Arm Limited and affiliates.
 * Copyright (c) 2021-     Dr.O.Hagendorf.
 * SPDX-License-Identifier: Apache-2.0
 */

#define HTTP_RECEIVE_BUFFER_SIZE 2048

#if !DEVICE_QSPI
#error [NOT_SUPPORTED] QSPI not supported for this target
#endif


#include "mbed.h"
#include <stdio.h>
#include <algorithm>
#include "QSPIFBlockDevice.h"
#include "SlicingBlockDevice.h"
#include "TDBStore.h"
#include "LittleFileSystem.h"

#include "network-helper.h"
#include "EthernetInterface.h"
#include "EthInterface.h"
#include "http_server.h"
#include "http_response_builder.h"
#include "multipart_parser.h"
#include "multipart_reader.h"

#include "CommandParser.h"

#include <string>
#include <cstring>
#include <sstream>

//################  VERSION  ###########################################
string    Version = "1.0";   // Programme version, see change log at end
//################ VARIABLES ###########################################

#define EXAMPLE_KV_VALUE_LENGTH 64
#define EXAMPLE_KV_KEY_LENGTH 32

#define BUF_LEN 80
typedef CommandParser<> MyCommandParser;

#define NUMSLICES 3

QSPIFBlockDevice bd(QSPI_FLASH1_IO0, QSPI_FLASH1_IO1, QSPI_FLASH1_IO2, QSPI_FLASH1_IO3,
                    QSPI_FLASH1_SCK, QSPI_FLASH1_CSN, QSPIF_POLARITY_MODE_0, MBED_CONF_QSPIF_QSPI_FREQ);
SlicingBlockDevice *slices[NUMSLICES];

TDBStore *nvStore;
LittleFileSystem fs("fs");

UnbufferedSerial rser(USBTX, USBRX);

SlicingBlockDevice *sliceMCUBoot;
SlicingBlockDevice *sliceFS;
SlicingBlockDevice *sliceKVStore;

NetworkInterface *network;

uint8_t idx=0;
uint8_t data[1];
uint8_t buffer0[80];
uint8_t buffer1[80];
volatile uint8_t NewData = 0;

MyCommandParser parser;
bool bStopMain = false;

DigitalOut led(LED1);

HttpServer *server;
string   webpage;
int downloadtime = 1, uploadtime = 1, downloadsize, uploadsize, downloadrate, uploadrate, numfiles;
Timer t;
void HTML_Home();
void HTML_Dir();
void HTML_System_Info();
void HTML_Delete(bool , const char *);
void Select_File_For_Function(string , string );
void UploadFileSelect();
string HTML_Header();
string HTML_Footer();
const char* get_mime_type(char* );
void parseContentX(string &, string &);
MultipartReader httpBodyReader;
string _itemName, _itemFilename, _itemType, _boundary;
bool _itemIsFile = false;
File fUpload;
enum enumHttpState {
  NONE,
  HANDLEUPLOAD
};
enumHttpState httpState;

void serialCb() {
  char ch;
  rser.read(&ch, 1);
  if (ch == 10 || ch == 13) {
    if (idx > 0) {
      NewData = 1;
      memcpy(buffer1, buffer0, idx);
      buffer1[idx++] = 0;
    } else {
      NewData = 0;
    }
    idx = 0;
  } else {
    if (idx < BUF_LEN) {
      buffer0[idx++] = ch;
    }
    NewData = 0;
  }
}

//################################### http server Start ##################################################
// http requests come in here
void request_handler(ParsedHttpRequest* request, TCPSocket* socket) {
 
  printf("Request came in: %s %s\n", http_method_str(request->get_method()), request->get_url().c_str());

  if (request->get_method() == HTTP_GET) {
    if(request->get_url() == "/") {
      HttpResponseBuilder builder(200);
      builder.set_header("Content-Type", "text/html; charset=utf-8");
      builder.set_header("Connection", "close");

      HTML_Home(); // Build webpage ready for display
      //printf("%s\n", webpage.c_str());

      builder.send(socket, webpage.c_str(), webpage.size());
    } else if(request->get_url() == "/icon") {
      //request->send(FS, "/icon.gif", "image/gif");
      File f;
      uint8_t *buffer;
      int bufferSize = 128;
      ssize_t  rsize, rsizeSum;
      if(f.open(&fs, "icon.gif")==0) {
        HttpResponseBuilder builder(200);
        builder.set_header("Content-Type", "application/octet-stream");
        builder.set_header("Content-Length", std::to_string(f.size()));
        string param = "attachment; filename=\"";
        param += "icon.gif";
        param += "\"";
        builder.set_header("Content-Disposition", param.c_str());

        if(isEthernet()) {
          bufferSize = ((EthernetInterface*)EthInterface::get_default_instance())->get_emac().get_mtu_size();
          if(bufferSize > 70) bufferSize -= 66;   // max data length = MTU - (EthernetHeader + IPHeader + TCPHeader)
          //printf("size %d\n", bufferSize);
          buffer = (uint8_t*)malloc(bufferSize);
        }
        buffer = (uint8_t*)malloc(bufferSize);

        rsize = f.read(buffer, bufferSize);
        rsizeSum = rsize;
        
        t.start(); 
        if(rsize == f.size()) {
          builder.send(socket, buffer, rsize);
          //printf("fertig %d\n", rsizeSum);
        }
        else {
          builder.send(socket, buffer, rsize);
          //printf("send %d %d\n", rsize, rsizeSum);
          while(rsize==bufferSize) {
            rsize = f.read(buffer, bufferSize);
            rsizeSum += rsize;
            socket->send(buffer, rsize);
            //printf("send %d %d\n", rsize, rsizeSum);
          }
          //printf("fertig %d\n", rsizeSum);
        }
        socket->send("\n", 1);
        t.stop();

        downloadtime = std::chrono::duration_cast<std::chrono::milliseconds>(t.elapsed_time()).count();
        downloadsize = f.size();

        f.close();
        free(buffer);
      }
      else {
        HttpResponseBuilder builder(404);
        builder.set_header("Connection", "close");
        builder.send(socket, NULL, 0);
      }
    }
    // ##################### DIR HANDLER ###############################
    else if(request->get_url() == "/dir") {
      printf("File Directory...\n");
      HttpResponseBuilder builder(200);
      builder.set_header("Content-Type", "text/html; charset=utf-8");
      builder.set_header("Connection", "close");

      HTML_Dir(); // Build webpage ready for display
      //printf("%s\n", webpage.c_str());

      builder.send(socket, webpage.c_str(), webpage.size());
    }
    // ##################### SYSTEM HANDLER ############################
    else if(request->get_url() == "/system") {
      HttpResponseBuilder builder(200);
      builder.set_header("Content-Type", "text/html; charset=utf-8");
      builder.set_header("Connection", "close");

      HTML_System_Info(); // Build webpage ready for display
      //printf("%s\n", webpage.c_str());

      builder.send(socket, webpage.c_str(), webpage.size());
    }
    // ##################### DOWNLOAD HANDLER ##########################
    else if(request->get_url() == "/download") {
      HttpResponseBuilder builder(200);
      builder.set_header("Content-Type", "text/html; charset=utf-8");
      builder.set_header("Connection", "close");

      //printf("Downloading file...\n");
      Select_File_For_Function("[DOWNLOAD]", "downloadhandler"); // Build webpage ready for display

      builder.send(socket, webpage.c_str(), webpage.size());
    }
    else if(request->get_url().find("/downloadhandler~/") == 0) {
      printf("Download handler started...\n");
      File f;
      uint8_t *buffer;
      int bufferSize = 128;
      ssize_t  rsize, rsizeSum;
      const char *fname;
      std::size_t found = request->get_url().find_last_of("/\\");
      if(found == string::npos) {
        HttpResponseBuilder builder(404);
        builder.set_header("Connection", "close");
        builder.send(socket, NULL, 0);
        return;
      }
      string fname1 = (request->get_url()).substr(found+1);
      fname = fname1.c_str();
      //printf("file: %s\n", fname);
      if(strlen(fname)==0) {
        HttpResponseBuilder builder(404);
        builder.set_header("Connection", "close");
        builder.send(socket, NULL, 0);
        return;
      }
 
      if(f.open(&fs, fname)) {
        HttpResponseBuilder builder(404);
        builder.set_header("Connection", "close");
        builder.send(socket, NULL, 0);
        return;
      }
      //printf("%s %ld\n", fname, f.size());
     
      HttpResponseBuilder builder(200);
      builder.set_header("Content-Type", "application/octet-stream");
      builder.set_header("Content-Length", std::to_string(f.size()));
      string param = "attachment; filename=\"";
      param += fname;
      param += "\"";
      builder.set_header("Content-Disposition", param.c_str());

      if(isEthernet()) {
        bufferSize = ((EthernetInterface*)EthInterface::get_default_instance())->get_emac().get_mtu_size();
        if(bufferSize > 70) bufferSize -= 66;   // max data length = MTU - (EthernetHeader + IPHeader + TCPHeader)
        //printf("size %d\n", bufferSize);
        buffer = (uint8_t*)malloc(bufferSize);
      }
      buffer = (uint8_t*)malloc(bufferSize);

      rsize = f.read(buffer, bufferSize);
      rsizeSum = rsize;
      
      t.start(); 
      if(rsize == f.size()) {
        builder.send(socket, buffer, rsize);
        //printf("fertig %d\n", rsizeSum);
      }
      else {
        builder.send(socket, buffer, rsize);
        //printf("send %d %d\n", rsize, rsizeSum);
        while(rsize==bufferSize) {
          rsize = f.read(buffer, bufferSize);
          rsizeSum += rsize;
          socket->send(buffer, rsize);
          printf("send %d %d\n", rsize, rsizeSum);
        }
        //printf("fertig %d\n", rsizeSum);
      }
      socket->send("\n", 1);
      t.stop();

      downloadtime = std::chrono::duration_cast<std::chrono::milliseconds>(t.elapsed_time()).count();
      downloadsize = f.size();

      f.close();
      free(buffer);

//      HttpResponseBuilder redirect(302);
//      redirect.set_header("Location", "/dir");
//      redirect.set_header("Connection", "close");
//      redirect.send(socket, NULL, 0);
    }
    // ##################### STREAM HANDLER ############################
    else if(request->get_url() == "/stream") {
      //printf("Streaming file...\n");
      HttpResponseBuilder redirect(302);
      redirect.set_header("Location", "/dir");
      redirect.set_header("Connection", "close");
      redirect.send(socket, NULL, 0);
      //Select_File_For_Function("[STREAM]", "streamhandler"); // Build webpage ready for display
      //request->send(200, "text/html", webpage);
    }
    // ##################### UPLOAD HANDLERS ###########################
    else if(request->get_url() == "/upload") {
      printf("Uploading file...\n");
      HttpResponseBuilder builder(200);
      builder.set_header("Content-Type", "text/html; charset=utf-8");
      builder.set_header("Connection", "close");

      UploadFileSelect(); // Build webpage ready for display
      //request->send(200, "text/html", webpage);
      builder.send(socket, webpage.c_str(), webpage.size());
    }
    // ##################### DELETE HANDLER ############################
    else if(request->get_url() == "/delete") {
      printf("Deleting file...\n");
      HttpResponseBuilder builder(200);
      Select_File_For_Function("[DELETE]", "deletehandler"); // Build webpage ready for display
      //request->send(200, "text/html", webpage);
      builder.send(socket, webpage.c_str(), webpage.size());
    }
    else if(request->get_url().find("/deletehandler") == 0) {
      std::size_t found = request->get_url().find_last_of("/\\");
      string fname1 = (request->get_url()).substr(found+1);
      const char *fname = fname1.c_str();
      printf("Delete handler started...%s\n", fname);

      HttpResponseBuilder builder(200);
      HTML_Delete(fs.remove(fname)==0, fname);

      builder.send(socket, webpage.c_str(), webpage.size());
    }
    else { // 404
      HttpResponseBuilder builder(404);
      builder.set_header("Connection", "close");
      builder.send(socket, NULL, 0);
    }
  }
  else if (request->get_method() == HTTP_POST) {
    if(request->get_url().find("/handleupload") == 0) {
      // when this is called everythings was already done
      HttpResponseBuilder redirect(302);
      redirect.set_header("Location", "/dir");
      redirect.set_header("Connection", "close");
      redirect.send(socket, NULL, 0);
    }
    else {
      HttpResponseBuilder builder(404);
      builder.set_header("Connection", "close");
      builder.send(socket, NULL, 0);
    }
  }
  else {
    HttpResponseBuilder builder(404);
    builder.set_header("Connection", "close");
    builder.send(socket, NULL, 0);
  }
}

void cbheadercomplete(HttpResponse* response) {
  printf("headercomplete_callback\n");
  printf("url: '%s'\n", response->get_url().c_str());
  if(response->get_url().find("/handleupload")==0)
    httpState = HANDLEUPLOAD;
  else
    httpState = NONE;
  
  if(httpState == HANDLEUPLOAD) {
    for (size_t ix = 0; ix < response->get_headers_length(); ix++) {
      string *key = response->get_headers_fields()[ix];
      string *value = response->get_headers_values()[ix];
      printf("\t%s: %s\n", key->c_str(), value->c_str());
      if(key->compare("Content-Type")==0)
        httpBodyReader.setBoundary(key, value);
    }
  }
}

void body_handler(const char *at, uint32_t length) {
  //printf("body: %d\n", length);
  size_t fed = 0;
  do {
    //printf("%d %d\n", fed, length);
    size_t ret = httpBodyReader.feed(at + fed, length - fed);
    fed += ret;
  } while (fed < length && !httpBodyReader.stopped());
}

void onPartBegin(const MultipartHeaders &headers, void *userData) {
  //printf("onPartBegin:\n");
  if(httpState == HANDLEUPLOAD) {
    MultipartHeaders::const_iterator it;
    MultipartHeaders::const_iterator end = headers.end();
    string key, value;
    for (it = headers.begin(); it != headers.end(); it++) {
      key = it->first; value = it->second;
      printf("  %s = %s\n", key.c_str(), value.c_str());
      if(key.compare("Content-Disposition")==0) {
        httpBodyReader.getFileInfos(key, value, _itemName, _itemFilename, _itemIsFile);
      }
      else if(key.compare("Content-Type")==0) {
        if(value.find("application/")!=string::npos) {
          _itemType = value;
        }
        else if(value.find("text/")!=string::npos) {
          _itemType = value;
        }
        else if(value.find("image/")!=string::npos) {
          _itemType = value;
        }
      }
    }
    //printf("name=%s, filename='%s', file=%d, type='%s'\n", _itemName.c_str(), _itemFilename.c_str(), _itemIsFile, _itemType.c_str());
    fUpload.open(&fs, _itemFilename.c_str(), O_CREAT|O_WRONLY);
    //uploadsize = 0;
    t.start();
  }
}
	
void onPartData(const char *buffer, size_t size, void *userData) {
  if(httpState == HANDLEUPLOAD) {
    fUpload.write(buffer, size);
  }
}

void onPartEnd(void *userData) {
  //printf("onPartEnd\n");
  if(httpState == HANDLEUPLOAD) {
    t.stop();
    uploadtime = std::chrono::duration_cast<std::chrono::milliseconds>(t.elapsed_time()).count();
    uploadsize = fUpload.size();
    fUpload.close();
  }
}

void onEnd(void *userData) {
  //printf("onEnd\n");
}

//################################### http server End ####################################################

//################################### command parser Start ###############################################
void cmd_stop(MyCommandParser::Argument *args, char *response) {
  bStopMain = true;
  printf("stop main - deinit/close blockdevices\n");
}

void cmd_getkey(MyCommandParser::Argument *args, char *response) {
  uint32_t val;
  int res = nvStore->get("_0000", &val, sizeof(val));
  printf("get result = %d, value = %d\n", res, val);
}

void cmd_listkey(MyCommandParser::Argument *args, char *response) {
  uint32_t val;
  int res;
  mbed::KVStore::iterator_t kvstore_it;
  char kv_key_out[EXAMPLE_KV_KEY_LENGTH] = {0};
  
  res = nvStore->iterator_open(&kvstore_it, NULL);
  memset(kv_key_out, 0, EXAMPLE_KV_KEY_LENGTH);
  //i_ind = 0;
  while (nvStore->iterator_next(kvstore_it, kv_key_out, EXAMPLE_KV_KEY_LENGTH) != MBED_ERROR_ITEM_NOT_FOUND) {
    res = nvStore->get(kv_key_out, &val, sizeof(val));
    printf("%s %d\n", kv_key_out, val);
    memset(kv_key_out, 0, EXAMPLE_KV_KEY_LENGTH);
  }
  res = nvStore->iterator_close(kvstore_it);
}

void cmd_setkey(MyCommandParser::Argument *args, char *response) {
  uint32_t val = (int32_t)args[0].asInt64;

  int res = nvStore->set("_0000", &val, sizeof(val), 0);
  printf("set result = %d\n", res);
}

void cmd_fsformat(MyCommandParser::Argument *args, char *response) {
  printf("formatting... ");
  fflush(stdout);
  int err = fs.reformat(sliceFS);
  printf("%s\n", (err ? "Fail :(" : "OK"));
  if (err) {
    error("error: %s (%d)\n", strerror(-err), err);
  }
}

void cmd_fsstatvfs(MyCommandParser::Argument *args, char *response) {
  struct statvfs buf;
  int err = statvfs("/fs/",  &buf);
  if (err < 0) {
    error("error: %s (%d)\n", strerror(errno), -errno);
  }
  printf("Filesystem block size %lu\n", buf.f_bsize);
  printf("Fragment size (block size) %lu\n", buf.f_frsize);
  printf("Number of blocks %llu\n", buf.f_blocks);
  printf("Number of free blocks %llu\n", buf.f_bfree);
  printf("Number of free blocks for unprivileged users %llu\n", buf.f_bavail);
  printf("Filesystem ID %lu\n", buf.f_fsid);
  printf("Maximum filename length %lu\n", buf.f_namemax);
}

void cmd_fsdir(MyCommandParser::Argument *args, char *response) {
  struct stat st;
  // Display the root directory
  printf("Opening the root directory... ");
  fflush(stdout);
  DIR *d = opendir("/fs/");
  char types[] = {'u', 'f', 'c', 'd', 'b', 'r', 'l', 's'};
  printf("%s\n", (!d ? "Fail :(" : "OK"));
  if (!d) {
    error("error: %s (%d)\n", strerror(errno), -errno);
  }

  printf("root directory:\n");
  while (true) {
    struct dirent *e = readdir(d);
    if (!e) {
      break;
    }
    int err = fs.stat(e->d_name, &st);
    if (err < 0) {
      error("error: %s (%d)\n", strerror(errno), -errno);
    }
    //printf("  %c %s %lu %ld %lu %lu\n", types[e->d_type], e->d_name, st.st_ino, st.st_size, st.st_atime, st.st_mtime);
    printf("  %c %s %ld\n", types[e->d_type], e->d_name, st.st_size);
  }

  printf("Closing the root directory... ");
  fflush(stdout);
  int err = closedir(d);
  printf("%s\n", (err < 0 ? "Fail :(" : "OK"));
  if (err < 0) {
    error("error: %s (%d)\n", strerror(errno), -errno);
  }
}

void cmd_fscat(MyCommandParser::Argument *args, char *response) {
  FILE *f;
  // Display the numbers file
  printf("Opening \"/fs/numbers.txt\"... ");
  fflush(stdout);
  f = fopen("/fs/numbers.txt", "r");
  printf("%s\n", (!f ? "Fail :(" : "OK"));
  if (!f) {
    error("error: %s (%d)\n", strerror(errno), -errno);
  }

  while (!feof(f)) {
    int c = fgetc(f);
    if(c!=EOF) printf("%c", c);
  }

  printf("\nClosing \"/fs/numbers.txt\"... ");
  fflush(stdout);
  int err = fclose(f);
  printf("%s\n", (err < 0 ? "Fail :(" : "OK"));
  if (err < 0) {
    error("error: %s (%d)\n", strerror(errno), -errno);
  }
}

void cmd_fscreate(MyCommandParser::Argument *args, char *response) {
  FILE *f;
  // Open the numbers file
  printf("Opening \"/fs/numbers.txt\"... ");
  fflush(stdout);
  f = fopen("/fs/numbers.txt", "r+");
  printf("%s\n", (!f ? "Fail :(" : "OK"));
  if (!f) {
    // Create the numbers file if it doesn't exist
    printf("No file found, creating a new file... ");
    fflush(stdout);
    f = fopen("/fs/numbers.txt", "w+");
    printf("%s\n", (!f ? "Fail :(" : "OK"));
    if (!f) {
        error("error: %s (%d)\n", strerror(errno), -errno);
    }
  }
 
  // Close the file which also flushes any cached writes
  printf("Closing \"/fs/numbers.txt\"... ");
  fflush(stdout);
  int err = fclose(f);
  printf("%s\n", (err < 0 ? "Fail :(" : "OK"));
  if (err < 0) {
    error("error: %s (%d)\n", strerror(errno), -errno);
  }
 }

void cmd_fswrite(MyCommandParser::Argument *args, char *response) {
  FILE *f;

  // Open the numbers file
  printf("Opening \"/fs/numbers.txt\"... ");
  fflush(stdout);
  f = fopen("/fs/numbers.txt", "r+");
  printf("%s\n", (!f ? "Fail :(" : "OK"));
  if (!f) {
    error("error: %s (%d)\n", strerror(errno), -errno);
  }

  int err = fseek(f, 0, SEEK_END);
  printf("%s\n", (!f ? "Fail :(" : "OK"));
  if (!f) {
    error("error: %s (%d)\n", strerror(errno), -errno);
  }

  err = fwrite(args[0].asString, sizeof(char), strlen(args[0].asString), f);
  printf("%s\n", (err < 0 ? "Fail :(" : "OK"));
  if (err < 0) {
      error("error: %s (%d)\n", strerror(errno), -errno);
  }
 
  // Close the file which also flushes any cached writes
  printf("Closing \"/fs/numbers.txt\"... ");
  fflush(stdout);
  err = fclose(f);
  printf("%s\n", (err < 0 ? "Fail :(" : "OK"));
  if (err < 0) {
      error("error: %s (%d)\n", strerror(errno), -errno);
  }
}

//################################### command parser End #################################################
int initHttpServer() {
  
  network = connect_to_default_network_interface();
  if (!network) {
      printf("Cannot connect to the network, see serial output\n");
      return 1;
  }

  server = new HttpServer(network);
  nsapi_error_t resHttp = server->init(8080, &request_handler, &cbheadercomplete, &body_handler);

  if (resHttp == NSAPI_ERROR_OK) {
    SocketAddress a;
    network->get_ip_address(&a);
    printf("Server is listening at http://%s:8080\n", a.get_ip_address() ? a.get_ip_address() : "None");
  }
  else {
      printf("Server could not be started... %d\n", resHttp);
    return 1;
  }
  
  httpBodyReader.onPartBegin = onPartBegin;
  httpBodyReader.onPartData = onPartData;
  httpBodyReader.onPartEnd = onPartEnd;
  httpBodyReader.onEnd = onEnd;

  return 0;
}

void initCommandParser() {
  parser.registerCommand("stop",     "",  "example:\n stop // deinit/close blockdevices", &cmd_stop);
  parser.registerCommand("getkey",   "",  "example:\n getkey",                            &cmd_getkey);
  parser.registerCommand("listkey",  "",  "example:\n listkey",                           &cmd_listkey);
  parser.registerCommand("setkey",   "i", "example:\n setkey 42",                         &cmd_setkey);
  parser.registerCommand("fsformat", "",  "example:\n fsformat",                          &cmd_fsformat);
  parser.registerCommand("fsdir",    "",  "example:\n fsdir",                             &cmd_fsdir);
  parser.registerCommand("fsstatvfs","",  "example:\n fsstatvfs",                         &cmd_fsstatvfs);
  parser.registerCommand("fscat",    "",  "example:\n fscat",                             &cmd_fscat);
  parser.registerCommand("fscreate", "",  "example:\n fscreate",                          &cmd_fscreate);
  parser.registerCommand("fswrite",  "s", "example:\n fswrite",                           &cmd_fswrite);
}


void initBlockdevices() {
  // Initialize the block device
  int err = bd.init();
  printf("bd.init -> %d\n", err);

  int erase_val = bd.get_erase_value();

  // Get device geometry
  bd_size_t read_size    = bd.get_read_size();
  bd_size_t program_size = bd.get_program_size();
  bd_size_t erase_size   = bd.get_erase_size();
  bd_size_t size         = bd.size();

  printf("--- Block device geometry ---\n");
  printf("read_size:    %lld B\n", read_size);
  printf("program_size: %lld B\n", program_size);
  printf("erase_size:   %lld B\n", erase_size);
  printf("size:         %lld B\n", size);
  printf("---\n");

  sliceMCUBoot = new SlicingBlockDevice(&bd, 0, 1048576);        // Create a block device that maps to the first 1Mb
  sliceFS      = new SlicingBlockDevice(&bd, 1048576, -16384);   // Create a block device that maps to next 3Mb-16kb
  sliceKVStore = new SlicingBlockDevice(&bd, -16384);            // Create a block device that maps to the last 16kb
  
  slices[0] = sliceMCUBoot;
  slices[1] = sliceFS;
  slices[2] = sliceKVStore;
  
  // Flash IC 4MByte
  // 1.Slice  1MByte für MCUBoot
  // 2.Slice  3Mbyte-16kByte für LittleFS
  // 3.Slice  16kByte für TDBStore
  for (int i = 0; i < NUMSLICES; i++) {
    // Initialize and erase the slice to prepar for programming
    slices[i]->init();
    // Get slice geometry
    bd_size_t read_size    = slices[i]->get_read_size();
    bd_size_t program_size = slices[i]->get_program_size();
    bd_size_t erase_size   = slices[i]->get_erase_size();
    bd_size_t size         = slices[i]->size();

    printf("--- Block device %d geometry ---\n", i);
    printf("read_size:    %lld B\n", read_size);
    printf("program_size: %lld B\n", program_size);
    printf("erase_size:   %lld B\n", erase_size);
    printf("size:         %lld B\n", size);
    printf("---\n");
  }
    
  //2.Slice für LittleFS initialisieren
  // Try to mount the filesystem
  printf("Mounting the filesystem... ");
  fflush(stdout);
  err = fs.mount(sliceFS);
  printf("%s (%d)\n", (err ? "Fail :(" : "OK"), err);
  if (err) {
    // Reformat if we can't mount the filesystem
    printf("formatting... ");
    fflush(stdout);
    err = fs.reformat(sliceFS);
    printf("%s (%d)\n", (err ? "Fail :(" : "OK"), err);
    if (err) {
      error("error: %s (%d)\n", strerror(-err), err);
    }
  }
  
  // 3.Slice für KVStore initialisieren
  nvStore = new TDBStore(sliceKVStore);
  int res = nvStore->init();
  printf("init result = %d\n", res);
}
// Entry point for the example
int main() {
  char response[MyCommandParser::MAX_RESPONSE_SIZE];
  int res, err;

  rser.baud(115200);
  rser.attach(&serialCb, SerialBase::RxIrq);

  printf("--- Mbed OS QSPIF block device and Http server example ---\n");

  initBlockdevices();
  
  uint32_t val;
  res = nvStore->get("_0000", &val, sizeof(val));
  printf("get result = %d, value = %d\n", res, val);

  val += 1;
  res = nvStore->set("_0000", &val, sizeof(val), 0);
  printf("set result = %d\n", res);

  initHttpServer();
  
  initCommandParser();
  parser.processCommand((const char*)"help", response);


  while(!bStopMain) {
    if(NewData) {
      parser.processCommand((const char*)buffer1, response);
      printf("%s\n", response);
      NewData = 0;
    }
    server->process();
  }

  // Tidy up
  printf("Unmounting fs... ");
  fflush(stdout);
  err = fs.unmount();
  printf("%s\n", (err < 0 ? "Fail :(" : "OK"));
  if (err < 0) {
      error("error: %s (%d)\n", strerror(-err), err);
  }

  // Deinitialize the block devices
  for (int i = 0; i < NUMSLICES; i++) {
    printf("slices[%d].deinit()\n", i);
    err = slices[i]->deinit();
    printf("slices[i].deinit -> %d\n", err);
  }

  printf("bd.deinit()\n");
  err = bd.deinit();
  printf("bd.deinit -> %d\n", err);

  printf("--- done! ---\n");
}

//############################### html build up ###############################################
void HTML_Home() {
  webpage = HTML_Header();
  webpage += "<h1>Home Page</h1>";
  webpage += "<h2>Mbed OS WebServer Example</h2>";
  webpage += "<img src = 'icon' alt='icon'>";
  webpage += "<h3>File Management - Directory, Upload, Download, Stream and Delete File Examples</h3>";
  webpage += HTML_Footer();
}
void HTML_Dir() {
  DIR *d;
  struct stat st;

  webpage  = HTML_Header();
  webpage += "<h3>File System Content</h3><br>";

  d = opendir("/fs/");
  char types[] = {'u', 'f', 'c', 'd', 'b', 'r', 'l', 's'};
  //printf("%s\n", (!d ? "Fail :(" : "OK"));
  if (!d) {
    error("error: %s (%d)\n", strerror(errno), -errno);
    webpage += "<h2>No Files Found</h2>";
    webpage += HTML_Footer();
    return;
  }

  //printf("root directory:\n");
  webpage += "<table class='center'>";
  webpage += "<tr><th>Type</th><th>File Name</th><th>File Size</th>";
  webpage += "</tr>\n";
  while (true) {
    struct dirent *e = readdir(d);
    if (!e) {
      break;
    }
    int err = fs.stat(e->d_name, &st);
    if (err < 0) {
      error("error: %s (%d)\n", strerror(errno), -errno);
    }
    //printf("  %c %s %ld\n", types[e->d_type], e->d_name, st.st_size);

    webpage += "<tr>";
    webpage += "<td style = 'width:5%'>";
    webpage += types[e->d_type]; 
    webpage += "</td>";
    webpage += "<td style = 'width:25%'>";
    webpage += e->d_name;
    webpage += "</td>";
    webpage += "<td style = 'width:10%'>";
    webpage += std::to_string(st.st_size);
    webpage += "</td>";
    webpage += "<td class='sp'></td>";
    webpage += "</tr>\n";
  }
  webpage += "</table>";
  webpage += "<p style='background-color:yellow;'><b></b></p>";

  //printf("Closing the root directory... ");
  fflush(stdout);
  int err = closedir(d);
  printf("%s\n", (err < 0 ? "Fail :(" : "OK"));
  if (err < 0) {
    error("error: %s (%d)\n", strerror(errno), -errno);
  }

  webpage += HTML_Footer();
}

void HTML_System_Info() {
  std::stringstream ss;
  SocketAddress a,b,c;
  uint32_t val;
  int res;
  mbed::KVStore::iterator_t kvstore_it;
  char kv_key_out[EXAMPLE_KV_KEY_LENGTH] = {0};

  network->get_ip_address(&a);
  char *ipaddr = a.get_ip_address() ? (char*)a.get_ip_address() : (char*)"None";
  char *macaddr = (char*)network->get_mac_address();
  network->get_netmask(&b);
  char *netmask = b.get_ip_address() ? (char*)b.get_ip_address() : (char*)"None";
  network->get_gateway(&c);
  char *netgw = c.get_ip_address() ? (char*)c.get_ip_address() : (char*)"None";
  int mtu = -1;
  
  if(isEthernet())
    mtu = ((EthernetInterface*)EthInterface::get_default_instance())->get_emac().get_mtu_size();

#if defined(MBED_SYS_STATS_ENABLED)
  mbed_stats_sys_t stats;
  mbed_stats_sys_get(&stats);
#endif

  webpage = HTML_Header();
  webpage += "<h3>System Information</h3>";
  webpage += "<h4>Transfer Statistics</h4>";
  webpage += "<table class='center'>";

  webpage += "<tr><th>Last Upload</th><th>Last Download/Stream</th><th>Units</th></tr>";
  webpage += "<tr><td>" + std::to_string(uploadsize) + "</td><td>" + std::to_string(downloadsize) + "</td><td>File Size</td></tr> ";
  webpage += "<tr><td>" + std::to_string((float)uploadsize / uploadtime * 1024.0) + "/Sec</td>";
  webpage += "<td>" + std::to_string((float)downloadsize / downloadtime * 1024.0) + "/Sec</td><td>Transfer Rate</td></tr>";
  webpage += "</table>";

  // #### Block Devices ####
  webpage += "<h4>Block Devices</h4>";
  webpage += "<table class='center'>";
  webpage += "<tr><th>Device</th><th>Total Space</th></tr>";
  webpage += "<tr><td>Main Device</td>";
  webpage += "<td>0x";
  ss.width(8); ss.fill('0');
  ss << std::hex << bd.size();
  webpage += ss.str(); ss.str("");
  webpage += "</td></tr>";
  webpage += "<tr><td>MCUBoot Slice</td>";
  webpage += "<td>0x";
  ss.width(8); ss.fill('0');
  ss << std::hex << sliceMCUBoot->size();
  webpage += ss.str(); ss.str("");
  webpage += "</td></tr>";
  webpage += "<tr><td>littleFS Slice</td>";
  webpage += "<td>0x";
  ss.width(8); ss.fill('0');
  ss << std::hex << sliceFS->size();
  webpage += ss.str(); ss.str("");
  webpage += "</td></tr>";
  webpage += "<tr><td>KVStore Slice</td>";
  webpage += "<td>0x";
  ss.width(8); ss.fill('0');
  ss << std::hex << sliceKVStore->size();
  webpage += ss.str(); ss.str("");
  webpage += "</td></tr>";
  webpage += "</table>";

  // #### KVStore ####
  res = nvStore->iterator_open(&kvstore_it, NULL);
  memset(kv_key_out, 0, EXAMPLE_KV_KEY_LENGTH);
  webpage += "<h4>TDBStore</h4>";
  webpage += "<table class='center'>";
  webpage += "<tr><th>Key</th><th>Value</th></tr>";
  while (nvStore->iterator_next(kvstore_it, kv_key_out, EXAMPLE_KV_KEY_LENGTH) != MBED_ERROR_ITEM_NOT_FOUND) {
    res = nvStore->get(kv_key_out, &val, sizeof(val));
    //printf("%s %d\n", kv_key_out, val);
    webpage += "<tr><td>";
    webpage += kv_key_out;
    webpage += "</td></tr>";
    memset(kv_key_out, 0, EXAMPLE_KV_KEY_LENGTH);
  }
  res = nvStore->iterator_close(kvstore_it);
  webpage += "</table>";

  // #### CPU&System Information ####
  webpage += "<h4>CPU&System Information</h4>";
  webpage += "<table class='center'>";
  webpage += "<tr><th>Parameter</th><th>Value</th></tr>";

#if defined(MBED_SYS_STATS_ENABLED)
  //printf("Mbed OS Version: %d\n", stats.os_version);
  webpage += "<tr><td>Mbed OS Version</td><td>";
  webpage += std::to_string(stats.os_version); 
  webpage += "</td></tr>";

  /* CPUID Register information
  [31:24]Implementer      0x41 = ARM
  [23:20]Variant          Major revision 0x0  =  Revision 0
  [19:16]Architecture     0xC  = Baseline Architecture
                          0xF  = Constant (Mainline Architecture)
  [15:4]PartNO            0xC20 =  Cortex-M0
                          0xC60 = Cortex-M0+
                          0xC23 = Cortex-M3
                          0xC24 = Cortex-M4
                          0xC27 = Cortex-M7
                          0xD20 = Cortex-M23
                          0xD21 = Cortex-M33
  [3:0]Revision           Minor revision: 0x1 = Patch 1
  */
  //printf("CPU ID: 0x%x\n", stats.cpu_id);
  webpage += "<tr><td>CPU ID</td><td>";
  //webpage += std::to_string(stats.cpu_id);
  webpage += "Implementer:"; webpage += ((stats.cpu_id>>24)&0xff)==0x41 ? "ARM" : std::to_string((stats.cpu_id>>24)&0xff);
  webpage += "<br>";
  webpage += "Variant:"; webpage += std::to_string((stats.cpu_id>>20)&0xf);
  webpage += "<br>";
  webpage += "Architecture:"; webpage += ((stats.cpu_id>>16)&0xf)==0xC ? "Baseline" : ((stats.cpu_id>>16)&0xf)==0xF ? "Mainline" : std::to_string((stats.cpu_id>>16)&0xf);
  webpage += "<br>";
  webpage += "CPUCore:";
  switch((stats.cpu_id>>4)&0xfff) {
    case 0xC20:
      webpage += "M0"; break;
    case 0xC60:
      webpage += "M0+"; break;
    case 0xC23:
      webpage += "M3"; break;
    case 0xC24:
      webpage += "M4"; break;
    case 0xC27:
      webpage += "M7"; break;
    case 0xD20:
      webpage += "M23"; break;
    case 0xD21:
      webpage += "M33"; break;
    default:
      webpage += std::to_string((stats.cpu_id>>4)&0xfff); break;
  }
  webpage += "<br>";
  webpage += " Revision:"; webpage += std::to_string(stats.cpu_id&0xf);
  webpage += "</td></tr>";

  /* Compiler IDs: ARM = 1; GCC_ARM = 2; IAR = 3 */
  //printf("Compiler ID: %d \n", stats.compiler_id);
  webpage += "<tr><td>Compiler</td><td>";
  switch(stats.compiler_id) {
    case 1:
      webpage += "ARM"; break;
    case 2:
      webpage += "GCC_ARM"; break;
    case 3:
      webpage += "IAR"; break;
    default:
      webpage += "unknown:";
      webpage += std::to_string(stats.compiler_id);
      break;
  }
  webpage += "</td></tr>";

  /* Compiler versions:
     ARM: PVVbbbb (P = Major; VV = Minor; bbbb = build number)
     GCC: VVRRPP  (VV = Version; RR = Revision; PP = Patch)
     IAR: VRRRPPP (V = Version; RRR = Revision; PPP = Patch)
  */
  //printf("Compiler Version: %d\n", stats.compiler_version);
  webpage += "<tr><td>Compiler Version</td><td>";
  switch(stats.compiler_id) {
    case 1:
      webpage += std::to_string(stats.compiler_version/1000000);
      webpage += ".";
      webpage += std::to_string((stats.compiler_version/10000)%100);
      webpage += ".";
      webpage += std::to_string(stats.compiler_version%10000);
      break;
    case 2:
      webpage += std::to_string(stats.compiler_version/10000);
      webpage += ".";
      webpage += std::to_string(stats.compiler_version%10000);
    case 3:
      webpage += std::to_string(stats.compiler_version/1000000);
      webpage += ".";
      webpage += std::to_string((stats.compiler_version/1000)%1000);
      webpage += ".";
      webpage += std::to_string(stats.compiler_version%1000);
    default:
      webpage += "unknown:";
      webpage += std::to_string(stats.compiler_version); break;
  }
  webpage += "</td></tr>";
  /* RAM / ROM memory start and size information */
  for (int i = 0; i < MBED_MAX_MEM_REGIONS; i++) {
    if (stats.ram_size[i] != 0) {
      //printf("RAM%d: Start 0x%d Size: 0x%d\n", i, stats.ram_start[i], stats.ram_size[i]);
      webpage += "<tr><td>RAM";
      webpage += std::to_string(i);
      webpage += "</td><td>Start: 0x";
      ss.width(8); ss.fill('0');
      ss << std::hex << stats.ram_start[i];
      webpage += ss.str(); ss.str("");
      webpage += " Size: 0x";
      ss << std::hex << stats.ram_size[i];
      webpage += ss.str(); ss.str("");
      webpage += "</td></tr>";
    }
  }
  webpage += "</td></tr>";
  for (int i = 0; i < MBED_MAX_MEM_REGIONS; i++) {
    if (stats.rom_size[i] != 0) {
      //printf("ROM%d: Start 0x%d Size: 0x%d\n", i, stats.rom_start[i], stats.rom_size[i]);
      webpage += "<tr><td>ROM";
      webpage += std::to_string(i);
      webpage += "</td><td>Start: 0x";
      ss.width(8); ss.fill('0');
      ss << std::hex << stats.rom_start[i];
      webpage += ss.str(); ss.str("");
      webpage += " Size: 0x";
      ss << std::hex << stats.rom_size[i];
      webpage += ss.str(); ss.str("");
      webpage += "</td></tr>";
    }
  }
  webpage += "</td></tr>";
#endif
  webpage += "</table>";

  // #### Network Information ####
  webpage += "<h4>Network Information</h4>";
  webpage += "<table class='center'>";
  webpage += "<tr><th>Parameter</th><th>Value</th></tr>";
  webpage += "<tr><td>MAC Address</td><td>";
  webpage += macaddr; 
  webpage += "</td></tr>";
  webpage += "<tr><td>MTU Size</td><td>";
  webpage += mtu==-1 ? "unknown" : std::to_string(mtu);
  webpage += "</td></tr>";
  webpage += "<tr><td>IP Address</td><td>";
  webpage += ipaddr; 
  webpage += "</td></tr>";
  webpage += "<tr><td>IP Mask</td><td>";
  webpage += netmask; 
  webpage += "</td></tr>";
  webpage += "<tr><td>IP Gateway</td><td>";
  webpage += netgw; 
  webpage += "</td></tr>";
  webpage += "</table> ";
  webpage += HTML_Footer();
}

void HTML_Delete(bool bDeleted, const char *fname) {
  webpage = HTML_Header();
  if(bDeleted) {
    webpage += "<h3>File '";
    webpage += fname;
    webpage += "' has been deleted</h3>";
    webpage += "<a href='/delete'>goto [Delete]</a><br><br>";
    webpage += "<a href='/dir'>goto [Dir]</a><br><br>";
    webpage += "<a href='/'>goto [Home]</a><br><br>";
  }
  else {
    webpage += "<h3>File [ ";
    webpage += fname;
    webpage += " ] was not deleted</h3>";
    webpage += "<a href='/delete'>goto [Delete]</a><br><br>";
    webpage += "<a href='/dir'>goto [Dir]</a><br><br>";
    webpage += "<a href='/'>goto [Home]</a><br><br>";
  }
  webpage  += HTML_Footer();
}
void Select_File_For_Function(string title, string function) {
  string Fname1, Fname2;
  int index = 0;
  DIR *d;
  struct stat st;

  webpage = HTML_Header();
  webpage += "<h3>Select a File to " + title + " from this device</h3>";
  webpage += "<table class='center'>";
  webpage += "<tr><th>File Name</th><th>File Size</th><th class='sp'></th></tr>";

  d = opendir("/fs/");
  char types[] = {'u', 'f', 'c', 'd', 'b', 'r', 'l', 's'};
  //printf("%s\n", (!d ? "Fail :(" : "OK"));
  if (!d) {
    error("error: %s (%d)\n", strerror(errno), -errno);
    webpage += "<h2>No Files Found</h2>";
    webpage += HTML_Footer();
    return;
  }

  while (true) {
    struct dirent *e = readdir(d);
    if (!e) {
      break;
    }
    if(e->d_name[0] == '.')
      continue;
    int err = fs.stat(e->d_name, &st);
    if (err < 0) {
      error("error: %s (%d)\n", strerror(errno), -errno);
    }
    //printf("  %c %s %ld\n", types[e->d_type], e->d_name, st.st_size);

    webpage += "<tr>";
    webpage += "<td style='width:25%'><button><a href='" + function + "~/" + e->d_name + "'>" + e->d_name + "</a></button></td><td style = 'width:10%'>" + std::to_string(st.st_size) + "</td>";
    webpage += "<td class='sp'></td>";
    webpage += "</tr>";
    index = index + 2;
  }

  webpage += "</table>";
  webpage += HTML_Footer();
}
void UploadFileSelect() {
  webpage  = HTML_Header();
  webpage += "<h3>Select a File to [UPLOAD] to this device</h3>";
  webpage += "<form method = 'POST' action = '/handleupload' enctype='multipart/form-data'>";
  webpage += "<input type='file' name='filename'><br><br>";
  webpage += "<input type='submit' value='Upload'>";
  webpage += "</form>";
  webpage += HTML_Footer();
}

string HTML_Header() {
  string page;
  page  = "<!DOCTYPE html>";
  page += "<html lang = 'en'>";
  page += "<head>";
  page += "<title>Web Server</title>";
  page += "<meta charset='UTF-8'>"; // Needed if you want to display special characters like the ° symbol
  page += "<style>";
  page += "body {width:75em;margin-left:auto;margin-right:auto;font-family:Arial,Helvetica,sans-serif;font-size:16px;color:blue;background-color:#e1e1ff;text-align:center;}";
  page += "footer {padding:0.08em;background-color:cyan;font-size:1.1em;}";
  page += "table {font-family:arial,sans-serif;border-collapse:collapse;width:70%;}"; // 70% of 75em!
  page += "table.center {margin-left:auto;margin-right:auto;}";
  page += "td, th {border:1px solid #dddddd;text-align:left;padding:8px;}";
  page += "tr:nth-child(even) {background-color:#dddddd;}";
  page += "h4 {color:slateblue;font:0.8em;text-align:left;font-style:oblique;text-align:center;}";
  page += ".center {margin-left:auto;margin-right:auto;}";
  page += ".topnav {overflow: hidden;background-color:lightcyan;}";
  page += ".topnav a {float:left;color:blue;text-align:center;padding:0.6em 0.6em;text-decoration:none;font-size:1.3em;}";
  page += ".topnav a:hover {background-color:deepskyblue;color:white;}";
  page += ".topnav a.active {background-color:lightblue;color:blue;}";
  page += ".notfound {padding:0.8em;text-align:center;font-size:1.5em;}";
  page += ".left {text-align:left;}";
  page += ".medium {font-size:1.4em;padding:0;margin:0}";
  page += ".ps {font-size:0.7em;padding:0;margin:0}";
  page += ".sp {background-color:silver;white-space:nowrap;width:2%;}";
  page += "</style>";
  page += "</head>";
  page += "<body>";
  page += "<div class = 'topnav'>";
  page += "<a href='/'>Home</a>";
  page += "<a href='/dir'>Directory</a>";
  page += "<a href='/upload'>Upload</a> ";
  page += "<a href='/download'>Download</a>";
  //page += "<a href='/stream'>Stream</a>";
  page += "<a href='/delete'>Delete</a>";
  //page += "<a href='/rename'>Rename</a>";
  page += "<a href='/system'>Status</a>";
  //page += "<a href='/format'>Format FS</a>";
  //page += "<a href='/newpage'>New Page</a>";
  //page += "<a href='/logout'>[Log-out]</a>";
  page += "</div>";
  return page;
}
string HTML_Footer() {
  string page;
  page += "<br><br><footer>";
  page += "<p class='medium'>Mbed OS WebServer Example</p>";
  page += "<p class='ps'><i>Copyright &copy;&nbsp;D L Bird 2021 Version 1.0</i></p>";
  page += "<p class='ps'><i>Copyright &copy;&nbsp;Dr.O.Hagendorf 2021 Version " +  Version + "</i></p>";
  page += "</footer>";
  page += "</body>";
  page += "</html>";
  return page;
}

const char* get_mime_type(char* filename ) {
	char *extension = strrchr( filename, '.' );   //string nach dem letzten .
	if (extension !=NULL) {
		if (strcasecmp(extension,".htm")==0  || strcasecmp(extension,".html") ==0)
		return "text/html; charset=utf-8";
		if (strcasecmp(extension,".csv")==0)
		return "text/comma-separated-values";
		if (strcasecmp(extension,".wav")==0)
		return "application/octet-stream";
		if (strcasecmp(extension,".bin")==0)
		return "application/octet-stream";
		if (strcasecmp(extension,".jpg")==0 || strcasecmp(extension,".jpeg" )==0)
		return "image/jpeg";
	}
	return "text/plain; charset=utf-8";
}
