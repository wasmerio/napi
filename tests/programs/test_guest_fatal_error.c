#include "node_api.h"

int main(void) {
  static const char location[] = "guest-module";
  static const char message[] = {'b', 'a', 'd', '\0', 'i', 'n', 'p', 'u', 't'};
  napi_fatal_error(location, sizeof(location) - 1, message, sizeof(message));
  return 0;
}
