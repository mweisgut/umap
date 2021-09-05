#include "UmapClientManager.hpp"  

long init_client_uffd() {
  struct uffdio_api uffdio_api;
  long uffd;

  /* Create and enable userfaultfd object */
  uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
  if (uffd == -1)
    perror("userfaultfd");

  uffdio_api.api = UFFD_API;
  uffdio_api.features = 0;
  if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
    perror("ioctl-UFFDIO_API");

  return uffd;
}

int UmapServInfo::setup_remote_umap_handle(){
  int status = 0;
  ActionParam params;
  params.act = uffd_actions::umap;
  strcpy(params.name, filename.c_str());
  params.args = args;
  void *umap_loc;

  ::write(umap_server_fd, &params, sizeof(params));
  // recieve memfd and region size
  sock_fd_read(umap_server_fd, &(loc), sizeof(region_loc), &(memfd));
  std::cout<<"c: recv memfd ="<<memfd<<" sz ="<<std::hex<<loc.size<<std::dec<<loc.page_size<<std::endl;
  umap_loc = (void *)get_umap_aligned_base_addr(loc.base_addr, loc.page_size);
  loc.len_diff = (uint64_t)umap_loc - (uint64_t)loc.base_addr;
  loc.base_addr = mmap(umap_loc, get_mmap_size(loc.size, loc.page_size), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, memfd, 0);
  if ((int64_t)loc.base_addr == -1) {
    perror("setup_uffd: map failed");
    exit(1);
  }

  std::cout<<"mmap:"<<std::hex<< loc.base_addr<<std::dec<<std::endl;
  //Tell server that the mmap is complete
  umap_loc = loc.base_addr + loc.len_diff;
  std::cout<<"Sending umap aligned address:"<<std::hex<<umap_loc<<std::dec<<std::endl;
  ::write(umap_server_fd, (void *)&umap_loc, sizeof(umap_loc));

  //Wait for the server to register the region to uffd
  sock_recv(umap_server_fd, (char*)&status, 1);
  std::cout<<"Done registering file to uffd"<<std::endl;
  return 0;
}

void UmapServInfo::remove_remote_umap_handle()
{
  int status = 0;
  ActionParam params;
  params.act = uffd_actions::unmap;
  strcpy(params.name, filename.c_str());
  ::write(umap_server_fd, &params, sizeof(params));
  sock_recv(umap_server_fd, (char*)&status, 1);
  std::cout<<"Done removing the uffd handling"<<std::endl;
  munmap(loc.base_addr, loc.size);
}

ClientManager* ClientManager::instance = NULL;

void ClientManager::setupUmapConnection(){
  int uffd;
  int dummy;
  if(!umap_server_fd){
    if(setup_uds_connection(&umap_server_fd, umap_server_path.c_str()) < 0){
      std::cout<<"Umap-Client Error: unable to setup connection with file server"<<std::endl;
      return;
    }
    uffd = init_client_uffd();
    sock_fd_write(umap_server_fd, &dummy, sizeof(int), uffd);
    ::close(uffd);
  }
}

void ClientManager::closeUmapConnection(){
  //Ideally this shouldn't require locks to be checked
  std::lock_guard<std::mutex> lock(cm_mutex);
  ::close(umap_server_fd);
}


UmapServInfo* ClientManager::cs_umap(std::string filename, int prot, int flags){
  umap_file_params args = {.prot = prot, .flags = flags};
  int dummy=0;
  int uffd=0;
  
  UmapServInfo *ret = NULL;
  if(file_conn_map.find(filename)!=file_conn_map.end()){ 
    //Todo:For multiple requests from multiple threads, it will need to be serialized
    std::cout<<"Umap-client Error: file already mapped for the application"<<std::endl;
    exit(-1);
  }else{
    ret = new UmapServInfo(umap_server_fd, filename, args);
    file_conn_map[filename] = ret;
  }
  return ret;
}
      
void ClientManager::cs_uunmap(std::string filename){
  auto it = file_conn_map.find(filename);
  if(it == file_conn_map.end()){
    std::cout<<"Umap-client Error: No file mapped with the filename"<<std::endl;
    exit(-1);
  }else{
    UmapServInfo* elem = it->second;
    file_conn_map.erase(it);
    delete elem;
  }
}

void* ClientManager::map_req(std::string filename, int prot, int flags){
  std::lock_guard<std::mutex> guard(cm_mutex);
  auto info = cs_umap(filename, prot, flags);
  if(info){
    return info->loc.base_addr + info->loc.len_diff;
  }else
    return NULL;
}

int ClientManager::unmap_req(std::string filename){
  std::lock_guard<std::mutex> guard(cm_mutex);
  auto it = file_conn_map.find(filename);
  if(it==file_conn_map.end()){
    std::cout<<"Umap-client Fault: unable to find connection with file server"<<std::endl;
    return -1;
  }else{
    //TODO: Has to submit the unmap request to the server
    cs_uunmap(filename);
  }
}
#ifdef __cplusplus
extern "C" {
#endif
//Umap client interface functions --- start
void init_umap_client(std::string sock_path){
  ClientManager *cm = ClientManager::getInstance(sock_path);
  cm->setupUmapConnection();
}

void *client_umap(const char *filename, int prot, int flags){
  ClientManager *cm = ClientManager::getInstance();
  return cm->map_req(std::string(filename), prot, flags);   
}

int client_uunmap(const char *filename){
  ClientManager *cm = ClientManager::getInstance();
  cm->unmap_req(std::string(filename));
  return 0;
}

void close_umap_client(){
  ClientManager *cm = ClientManager::getInstance();
  cm->closeUmapConnection();
  ClientManager::deleteInstance();
}
#ifdef __cplusplus
}
#endif
//End of Umap Client interface functions -- end