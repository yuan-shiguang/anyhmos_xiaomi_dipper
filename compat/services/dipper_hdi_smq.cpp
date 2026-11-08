/* SPDX-License-Identifier: MIT
 * Real shared-memory prerequisite for the original HDI command transport.
 * Does not submit frames or alter display power.
 */
#include "display/composer/v1_3/idisplay_composer.h"
#include <cstdio>
#include <unistd.h>
using namespace OHOS::HDI::Base;
#define CHECK(x) do{if(!(x)){printf("HDI_SMQ_FAIL line=%d %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
    setbuf(stdout,nullptr);alarm(12);
    auto queue=std::make_shared<SharedMemQueue<int32_t>>(1024,SmqType::SYNCED_SMQ);
    printf("HDI_SMQ_LOCAL good=%d\n",queue->IsGood());CHECK(queue->IsGood());
    int32_t sent=0x53484e47,received=0;
    CHECK(queue->Write(&sent)==0);CHECK(queue->Read(&received)==0&&received==sent);
    auto service=OHOS::HDI::Display::Composer::V1_3::IDisplayComposer::Get("display_composer_service",false);
    CHECK(service!=nullptr);int result=service->InitCmdRequest(queue);
    printf("HDI_SMQ_INIT_REQUEST result=%d\n",result);CHECK(result==0);
    std::shared_ptr<SharedMemQueue<int32_t>> reply;
    result=service->GetCmdReply(reply);printf("HDI_SMQ_REPLY result=%d valid=%d\n",result,reply&&reply->IsGood());
    CHECK(result==0&&reply&&reply->IsGood());
    puts("HDI_SMQ_RESULT=0 local_memory_roundtrip=1 original_proxy_transport=1 frame_submission=0");return 0;
}
