﻿/* A simple server in the internet domain using TCP
   The port number is passed as an argument */

/**
 * server 主要进行请求的分发，接收到新的请求就把请求分发给route，
 */
#include "server.h"

#ifdef _WIN32
#pragma comment(lib,"libtox.dll.a")
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"jansson.lib")
#endif
/**
 * Globals
 */
Tox *my_tox;
Queue *msg_task_queue; // 消息处队列
Queue *msg_rec_queue; //需要传递给node端的信息
Msg_listener_list *msg_listener_list = NULL;
uint8_t offline_count = 0;

// 队列等待条件锁
extern pthread_mutex_t msg_task_lock;
pthread_cond_t msg_task_cond = PTHREAD_COND_INITIALIZER;


const uint8_t *target_ip; // 远端连接目标IP
uint32_t target_port; // 远端连接目标端口
const uint8_t *target_id; // 远程目标ADDRESS
uint32_t local_port; //本地端口

// FRIEND NUMBER
uint32_t FRIEND_NUM = 0;

// flags
int32_t init_over_flag = 0;
int32_t init_req_flag = 0;

// local sockets
local_socks_list *msocks_list = NULL;
uint32_t local_socksfd = 0;

//debug
int previous = -1;
    
void error(const char *msg)
{
    perror(msg);
    exit(1);
}

void friend_message(Tox *m, int32_t friendnumber, const uint8_t *bin, uint16_t msglength, void *userdata) {
	uint8_t client_id_bin[TOX_CLIENT_ID_SIZE+1];
    uint8_t client_id_str[TOX_CLIENT_ID_SIZE*2+1];
    tox_get_client_id(m,friendnumber,client_id_bin);
    hex_bin_to_string(client_id_bin,TOX_CLIENT_ID_SIZE,client_id_str);
    // 添加消息觸發器
    trigger_msg_listener(msg_listener_list,bin,client_id_str);
        
    if(strcmp(bin,"HANDSHAKE") == 0){
        printf("HANDSHAKE RECEIVED\n");
        return;
    }
        
    // parse message
    uint8_t *uuid = (uint8_t *)malloc(sizeof(uint8_t)*UUID_LENGTH + 1);
	uint8_t *cmd = (uint8_t *)malloc(sizeof(uint8_t)*CMD_STR_LENGTH);
    uint8_t *data = (uint8_t *)malloc(sizeof(uint8_t)*MY_MESSAGE_LENGTH);
    uint32_t *length = (uint32_t *)malloc(sizeof(uint32_t));
    unpack_msg_bin(bin, uuid, cmd, data, length);
	// print msg content
	/*printf("uuid:%s\n", uuid);
	printf("cmd:%s\n", cmd);
	printf("data:%s\n", data);
	printf("length:%d\n", *length);*/
        
    if(strcmp(cmd,"UNKNOWN_CMD") == 0){
            
    }else if(strcmp(cmd,"RAW_DATA") == 0){
        on_remote_data_received(uuid, data, *length, client_id_bin);
    }else{
        printf("CMD:%s\n",cmd);
        if(strcmp(cmd,"CLOSE_SOCK") == 0){
            int32_t sockfd = get_local_socks(msocks_list,uuid);
            close_local_socks(msocks_list,sockfd);
        }
        if(strcmp(cmd, "CREATE_SOCK_SUCCESS") == 0){
            int32_t sockfd = get_local_socks(msocks_list,uuid);
			pthread_t new_sock_thread;
            uint32_t *msockfd = malloc(sizeof(uint32_t));
			*msockfd = sockfd;
            pthread_create(&new_sock_thread, NULL, on_remote_sock_created, (void *)msockfd);
        }
    }
        
    // free data
    free(uuid);
    free(cmd);
    free(data);
    free(length);
}


static Tox *init_tox()
{
    Tox *m = tox_new(1);

    if (m == NULL) {
        printf(stderr, "IPv6 didn't initialize, trying IPv4\n");
        m = tox_new(0);
    }

    if (m == NULL)
	printf(stderr, "Forcing IPv4 connection\n");

    tox_callback_friend_message(m, friend_message, NULL);

    tox_set_name(m, MY_NAME, strlen(MY_NAME)); // Sets the username
    return m;
}

int init_tox_connection(Tox *m)
{
    uint8_t pub_key[TOX_FRIEND_ADDRESS_SIZE];
    hex_string_to_bin(pub_key, BOOTSTRAP_KEY);
    int res = tox_bootstrap_from_address(m, BOOTSTRAP_ADDRESS, 0, htons(BOOTSTRAP_PORT), pub_key);
    if (!res) {
        exit(1);
    }
	return 0;
}



void *tox_works(void *a){
    // do tox loop
    time_t timestamp0 = time(NULL);
    int on = 0;
    while (1) {
        if (on == 0) {
            if (tox_isconnected(my_tox)) {
                on = 1;
            } else {
                time_t timestamp1 = time(NULL);
                if (timestamp0 + 10 < timestamp1) {
                    timestamp0 = timestamp1;
                    init_tox_connection(my_tox);
                }
                
            }
        }
        tox_do(my_tox);
        if(msg_task_queue->size != 0){
            send_data_remote();
        }else{
#ifdef _WIN32
			Sleep(4);
#else
			usleep(4000);
#endif
        }
    }
}



void intHandler(int dummy) {
    store_data(my_tox);
#ifdef _WIN32
	WSACleanup();
#endif
    printf("EXITING...\n");
    exit(EXIT_SUCCESS);
}


#ifdef _WIN32
int32_t init_local_sock_serv(uint32_t local_port){
    int32_t iServerSock;
    struct sockaddr_in ServerAddr;
    WSADATA WSAData;
    
    if(WSAStartup(MAKEWORD(1, 1), &WSAData)){
        printf("initializationing error!\n");
        exit(0);
    }

    if((iServerSock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET){
        printf("create socket failed\n");
        exit(0);
    }

    ServerAddr.sin_family = AF_INET;
    ServerAddr.sin_port = htons(local_port);//监视的端口号
    ServerAddr.sin_addr.s_addr = INADDR_ANY;//本地IP
    memset(& (ServerAddr.sin_zero), 0, sizeof(ServerAddr.sin_zero));
    
    if(bind(iServerSock, (struct sockaddr *)&ServerAddr, sizeof(struct sockaddr)) == -1){
        printf("bind failed!\n");
        exit(0);
    }

    if(listen(iServerSock, 5) == -1){
        printf("listen failed!\n");
        exit(0);
    }
    return iServerSock;
}

int init_local_sock(int local_port){
    
    int iClientSock;
    struct sockaddr_in ServerAddr;
    
    WSADATA WSAData;


    if(WSAStartup(MAKEWORD(1, 1), &WSAData)){
        printf("initializationing error!\n");
        exit(0);
    }

    if((iClientSock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET){
        printf("create socket failed!\n");
        exit(0);
    }

    ServerAddr.sin_family = AF_INET;
    ServerAddr.sin_port = htons(local_port);
    ServerAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    memset(&(ServerAddr.sin_zero), 0, sizeof(ServerAddr.sin_zero));

    if(connect(iClientSock, (struct sockaddr *) & ServerAddr, sizeof(struct sockaddr)) == -1){
        printf("connect failed");
        exit(0);
    }
    
    return iClientSock;
}

#else

int32_t init_local_sock(uint32_t local_port){
    struct sockaddr_in serv_addr;
    struct hostent *server;
    
    int32_t sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");
    server = gethostbyname("127.0.0.1");
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host\n");
        exit(0);
    }
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, 
         (char *)&serv_addr.sin_addr.s_addr,
         server->h_length);
    serv_addr.sin_port = htons(local_port);
    if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) 
        error("ERROR connecting");
    return sockfd;
}

int32_t init_local_sock_serv(uint32_t local_port){
    int sockfd;
    struct sockaddr_in serv_addr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(local_port);
    if (bind(sockfd, (struct sockaddr *) &serv_addr,
            sizeof(serv_addr)) < 0) 
            error("ERROR on binding");
    listen(sockfd,5);
    return sockfd;
}

#endif


int32_t readCSock(uint32_t sockfd,uint8_t *buf,uint32_t length){
#ifdef _WIN32
    return recv(sockfd, buf, length, 0);
#else
    return read(sockfd, buf, length);
#endif
}

int32_t writeCSock(uint32_t sockfd, const uint8_t *buf, uint32_t length){
#ifdef _WIN32
    return send(sockfd, buf, length, 0);
#else
    return write(sockfd,buf,length);
#endif
}



/**
 * add data to remote message queue
 * this func may block if current message queue is not available
 * data format
 * {
 *  target_id: target to send
 *  uuid: sock uuid
 *  data: raw data <SOCK_BUF_SIZE or less>
 *  cmd: can be null when no cmd is to send
 * }
 * 
 */
void write_data_remote(const uint8_t *uuid, const uint8_t* cmd, const uint8_t *data, const uint32_t length){
    // pack msg to bin
    uint8_t *msg_bin = (uint8_t *)malloc(sizeof(uint8_t)*MY_MESSAGE_LENGTH);
	pack_msg_bin(msg_bin, uuid, cmd, data, length);
	uint8_t hight_byte = msg_bin[UUID_LENGTH + CMD_LENGTH];
	uint8_t low_byte = msg_bin[UUID_LENGTH + CMD_LENGTH + 1];
	uint16_t mlength = hight_byte * 256 + low_byte;
    // print msg content
	/*printf("uuid:%s\n", uuid);
	printf("cmd:%s\n", cmd);
	printf("data:%s\n", data);
	printf("length:%d\n", length);
	printf("afterLength:%d\n", mlength);*/
	// unpack msg bin
	//debug_msg_bin(msg_bin);
    // add msg to message queue
    MSGTask *newTask = (MSGTask *)malloc(sizeof(MSGTask));
    uint8_t *target_addr_bin = (uint8_t *)malloc(sizeof(uint8_t)*TOX_FRIEND_ADDRESS_SIZE);
    hex_string_to_bin(target_addr_bin, target_id);
    newTask->target_addr_bin = target_addr_bin;
    newTask->msg = msg_bin;
    
    pthread_mutex_lock(&msg_task_lock);
    while ((msg_task_queue->size) >= MAX_MSG_CACHE){
        pthread_cond_wait(&msg_task_cond, &msg_task_lock);
    };
    // enter queue
    Enqueue(msg_task_queue, newTask);
    if((msg_task_queue->size) < MAX_MSG_CACHE)
        pthread_cond_broadcast(&msg_task_cond);
    pthread_mutex_unlock(&msg_task_lock);
    
    // free space
    free(msg_bin);
    free(target_addr_bin);
    free(newTask);
}




void debug_msg_bin(uint8_t *msg){
	uint8_t *uuid = (uint8_t *)malloc(sizeof(uint8_t)*UUID_LENGTH + 1);
	memset(uuid, '\0', UUID_LENGTH + 1);
	uint8_t *cmd = (uint8_t *)malloc(sizeof(uint8_t) * CMD_STR_LENGTH);
	uint8_t *data = (uint8_t *)malloc(sizeof(uint8_t)*MY_MESSAGE_LENGTH);
	uint32_t *length = (uint32_t *)malloc(sizeof(uint32_t));
	unpack_msg_bin(msg, uuid, cmd, data, length);
	// print msg content
	printf("uuid:%s\n", uuid);
	printf("cmd:%s\n", cmd);
	printf("data:%s\n", data);
	printf("length:%d\n", *length);
	free(uuid);
	free(cmd);
	free(data);
	free(length);
}

/**
 * read message from remote message queue, and send to remote
 * this will only send one message in the message queue
 * and write_data_remote will be block at the same time
 */
void send_data_remote(){
    MSGTask *mTask = front(msg_task_queue); // 开始处理
    int friend_num = tox_get_friend_number(my_tox,mTask->target_addr_bin);
    int res = -1;
    int retry_count = 0;
    //debug_msg_bin(mTask->msg);
    while(res <=0 && retry_count <5){
        res = tox_send_message(my_tox,friend_num,mTask->msg,MY_MESSAGE_LENGTH);
        retry_count += 1;
    }
    if(retry_count < 5){
        Dequeue(msg_task_queue);
        pthread_cond_broadcast(&msg_task_cond);
    }else{
        // check target online status
        if(tox_get_friend_connection_status(my_tox,friend_num) != 1){
            // target offline
            Dequeue(msg_task_queue);
            pthread_cond_broadcast(&msg_task_cond);
            // unpack msg to get uuid
            uint8_t *uuid = (uint8_t *)malloc(sizeof(uint8_t)*UUID_LENGTH+1);
            memset(uuid,'\0',UUID_LENGTH+1);
			uint8_t *cmd = (uint8_t *)malloc(sizeof(uint8_t)*CMD_STR_LENGTH);
            uint8_t *data = (uint8_t *)malloc(sizeof(uint8_t)*MY_MESSAGE_LENGTH);
            uint32_t *length = (uint32_t *)malloc(sizeof(uint32_t));
            unpack_msg_bin(mTask->msg, uuid, cmd, data, length);
            // get socket fd by uuid
            int32_t sockfd = get_local_socks(msocks_list,uuid);
            // close socket
            close_local_socks(msocks_list,sockfd);
            free(uuid);
            free(cmd);
            free(data);
            free(length);
            printf("target is offline. ******************************************************\n");
            printf("status %d\n", tox_get_friend_connection_status(my_tox,friend_num));
            printf("friend num %d\n", friend_num);
            //exit(1);
        }
    }
}

void debug_data(const uint8_t *data,uint32_t length){
    uint32_t i =0;
    if(previous != -1){
        if(data[0] != previous +1 && data[0] != 0){
            printf("%d,%d\n",previous,data[0]);
            printf("DATA ERROR 1\n");
            exit(1);
        }
    }
    for(i=0;i<length-1;i++){
        if(data[i+1] != data[i]+1 && data[i+1] !=0){
            printf("%d,%d\n",data[i],data[i+1]);
            printf("DATA ERROR\n");
            exit(1);
        }
    }
    previous = data[length-1];
}

/**
 * this func will be called immediately after local data received
 * wrap data in proper format
 * {
 *  uuid
 *  data
 * }
 */
void on_local_data_received(uint8_t *data, uint32_t length,uint32_t sockfd){
    uint8_t uuid[UUID_LENGTH+1];
    get_local_socks_uuid(msocks_list,sockfd,uuid);
    if(uuid[0] == '\0'){
        printf("on_local_data_received error\n");// Bug: this is still known
        return;
    }
    write_data_remote(uuid,NULL,data,length);
}

/**
 * this func will be called immediately after remote data received
 * NOTE: remote data is not remote message. it's remote message with fixed format
 * {
 *  uuid
 *  data
 *  cmd
 * }
 */
void on_remote_data_received(const uint8_t *uuid, const uint8_t *data, const uint32_t length, const uint8_t *client_id_bin){
    // get sockfd from uuid
    
    int32_t sockfd = get_local_socks(msocks_list,uuid);
    
    // send data to target socket
    if(sockfd != 0){
        // record found
		writeCSock(sockfd, data, length);
    }else{
        // socket might be closed
        printf("INVALID SOCKET, CLOSE IT\n");
        close_remote_socket(uuid,client_id_bin);
    }
}


/**
 * close remote socket
 */

void close_remote_socket(const uint8_t *uuid, const uint8_t *client_id_bin){
    if(uuid[0] == '\0'){
        printf("close_remote_socket error\n");
    }
    write_data_remote(uuid,"CLOSE_SOCK","",strlen(""));
}


void create_remote_socket(const uint8_t *uuid, const uint8_t *client_id_bin,const uint8_t *target_ip, uint32_t target_port){
    json_t *target_port_json = json_pack("i",target_port);
    json_t *target_ip_json = json_pack("s",target_ip);
    json_t *data = json_object();
    json_object_set(data,"target_ip",target_ip_json);
    json_object_set(data,"target_port",target_port_json);
    uint8_t *data_str = json_dumps(data,JSON_INDENT(4));
    if(uuid[0] == '\0'){
        printf("create_remote_socket error\n");
        exit(1);
    }
    write_data_remote(uuid,"CREATE_SOCK",data_str,strlen(data_str));
    
    // release data
    json_decref(target_port_json);
    json_decref(target_ip_json);
    json_decref(data);
    free(data_str);
}



void *on_remote_sock_created(void *msockfd){
    uint32_t sockfd = *((uint32_t *)msockfd);
    uint8_t *target_addr_bin = (uint8_t *)malloc(sizeof(uint8_t)*TOX_FRIEND_ADDRESS_SIZE);
    hex_string_to_bin(target_addr_bin,target_id);
    uint8_t uuid[UUID_LENGTH+1];
    get_local_socks_uuid(msocks_list,sockfd,uuid);
    printf("uuid: %s\n",uuid);
    printf("socketfd: %d\n",sockfd);
    uint8_t buf[SOCK_BUF_SIZE];
    int length = 1;
    while(length > 0){
        memset(buf,0,SOCK_BUF_SIZE);
        length = readCSock(sockfd,buf,SOCK_BUF_SIZE-1);
        if(length > 0){
            on_local_data_received(buf,length,sockfd);
        }
            
    }
    // read data error
    // close remote and local sock
	printf("uuid: %s\n", uuid);
	printf("socketfd: %d\n", sockfd);
    
    if(get_local_socks(msocks_list, uuid) != 0){
        close_remote_socket(uuid,target_addr_bin);// 从本地发起的关闭
    }else{
        // closed by remote before create success send
        if(uuid[0] == '\0'){
            //exit(0);// maybe local socket has been closed
            printf("closed by local before create socket received\n");
        }
    }
    close_local_socks(msocks_list, sockfd);
    free(target_addr_bin);
    free(msockfd);
    return NULL;
}

int main(int argc, char *argv[])
{
    // 添加系统事件监听
    signal(SIGINT, intHandler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    my_tox = init_tox();
    init_tox_connection(my_tox);
    load_data(my_tox);
    // 处理参数
    if(argc >3){
		local_port = atoi(argv[1]);
        target_id = argv[2];
        target_ip = argv[3];
        target_port = atoi(argv[4]);
        
        uint8_t my_address_bin[TOX_FRIEND_ADDRESS_SIZE+1];
        uint8_t my_address_str[TOX_FRIEND_ADDRESS_SIZE*2+1];
        tox_get_address(my_tox,my_address_bin);
        hex_bin_to_string(my_address_bin,TOX_FRIEND_ADDRESS_SIZE,my_address_str);
        printf("MYID:%s\n",my_address_str);
    }else{
		local_port = 9990 ;
		target_id = "3E567CBCE8DA8A18ED3C30127ADB61D78AFA2D01B5CD12425C110E84F23AC365144CE2807378";
		target_ip = "127.0.0.1";
		target_port = 22;
    }
    // 虛擬參數
    
    // 初始化消息隊列
    msg_task_queue = createQueue(MAX_MSG_CACHE); // 远程操作消息队列
    
    // 開始tox線程
    pthread_t tox_thread;
    int iret1 = pthread_create( &tox_thread, NULL, tox_works,NULL);
    if(iret1){
        exit(EXIT_FAILURE);
    }
    
    // 初始化本地连接
    local_socksfd = init_local_sock_serv(local_port);
    
    // 等待tox成功連接
    while(!tox_isconnected(my_tox)){
#ifdef _WIN32
		Sleep(20);
#else
		usleep(20000);
#endif
        
    }
    printf("TOXCORE:ONLINE\n");
    printf("SERVER:LISTEN ON %d\n",local_port);
    // 進入請求者模式
    int res = init_connect(my_tox,target_id,&msg_listener_list);
    if(res == 402){
        printf("CONNECT:OK\n");
    }
    else{
        printf("CONNECT:ERROR\n");
    }
    

    // client mode
    // create local tcp server
    msocks_list = create_local_socks_list();
    
    while(1){
        struct sockaddr_in cli_addr;
        uint32_t clilen = sizeof(cli_addr);
		int32_t newsockfd = accept(local_socksfd,
			(struct sockaddr *) &cli_addr,
			&clilen);
        
		if (newsockfd < 0){
			printf("socket error\n");
		}
		else{
			printf("accepted:%d\n", newsockfd);
			// 发送创建远程端口指令
            uint32_t sockfd = newsockfd;
            uint8_t *target_addr_bin = (uint8_t *)malloc(sizeof(uint8_t)*TOX_FRIEND_ADDRESS_SIZE);
            hex_string_to_bin(target_addr_bin,target_id);
            add_local_socks(msocks_list,sockfd,target_addr_bin,target_ip,target_port);
            // create remote socket
            uint8_t uuid[UUID_LENGTH+1];
            get_local_socks_uuid(msocks_list,sockfd,uuid);
            create_remote_socket(uuid,target_addr_bin,target_ip,target_port);
            free(target_addr_bin);
		}
    }
    
    printf("EXITED\n");
    return 0; 
}
