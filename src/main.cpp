#include <curl/curl.h>
#include <cstdlib>
#include <iostream>

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::atexit(curl_global_cleanup);

  std::cout << "pu-cli starting...\n";
  return 0;
}
