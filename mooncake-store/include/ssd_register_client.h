#include "master_client.h"
#include "types.h"
#include <string>

namespace mooncake {
   #define OPERATION_OK 0
   #define OPERATION_FAILED -1

class NoFRegisterClient {
   public:
      NoFRegisterClient();
      ~NoFRegisterClient();

      int set_register(const std::string &nqn, size_t nsid, const std::string &traddr, size_t trsvcid, uintptr_t base, size_t size, const std::string &master_server_addr);
 
   private: 
      MasterClient master_client_;
   };
}
