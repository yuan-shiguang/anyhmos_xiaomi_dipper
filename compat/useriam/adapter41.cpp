// Private dipper adaptation; generated signatures from pinned OpenHarmony IDL.
// AnyHarmonyOS software backend retains its upstream Apache-2.0 license.
#include "user_auth/v4_1/iuser_auth_interface.h"
#include <dlfcn.h>
#include <new>
using namespace OHOS;
using Base=HDI::UserAuth::V4_0::IUserAuthInterface;
class DipperUserAuth41 final : public HDI::UserAuth::V4_1::IUserAuthInterface {
 sptr<Base> base_;
public:
 explicit DipperUserAuth41(Base *base):base_(base){}
 int32_t BeginAuthenticationExt(uint64_t contextId, const OHOS::HDI::UserAuth::V4_1::AuthParamExt& param, std::vector<OHOS::HDI::UserAuth::V4_0::ScheduleInfo>& scheduleInfos) override {(void)contextId;(void)param;(void)scheduleInfos;return -8;}
 int32_t BeginEnrollmentExt(const std::vector<uint8_t>& authToken, const OHOS::HDI::UserAuth::V4_1::EnrollParamExt& param, OHOS::HDI::UserAuth::V4_0::ScheduleInfo& info) override {(void)authToken;(void)param;(void)info;return -8;}
 int32_t Init(const std::string& deviceUdid) override {return base_->Init(deviceUdid);}
 int32_t AddExecutor(const OHOS::HDI::UserAuth::V4_0::ExecutorRegisterInfo& info, uint64_t& index, std::vector<uint8_t>& publicKey, std::vector<uint64_t>& templateIds) override {return base_->AddExecutor(info,index,publicKey,templateIds);}
 int32_t DeleteExecutor(uint64_t index) override {return base_->DeleteExecutor(index);}
 int32_t OpenSession(int32_t userId, std::vector<uint8_t>& challenge) override {return base_->OpenSession(userId,challenge);}
 int32_t CloseSession(int32_t userId) override {return base_->CloseSession(userId);}
 int32_t UpdateEnrollmentResult(int32_t userId, const std::vector<uint8_t>& scheduleResult, OHOS::HDI::UserAuth::V4_0::EnrollResultInfo& info) override {return base_->UpdateEnrollmentResult(userId,scheduleResult,info);}
 int32_t CancelEnrollment(int32_t userId) override {return base_->CancelEnrollment(userId);}
 int32_t DeleteCredential(int32_t userId, uint64_t credentialId, const std::vector<uint8_t>& authToken, OHOS::HDI::UserAuth::V4_0::CredentialOperateResult& operateResult) override {return base_->DeleteCredential(userId,credentialId,authToken,operateResult);}
 int32_t GetCredential(int32_t userId, int32_t authType, std::vector<OHOS::HDI::UserAuth::V4_0::CredentialInfo>& infos) override {return base_->GetCredential(userId,authType,infos);}
 int32_t GetUserInfo(int32_t userId, uint64_t& secureUid, int32_t& pinSubType, std::vector<OHOS::HDI::UserAuth::V4_0::EnrolledInfo>& infos) override {return base_->GetUserInfo(userId,secureUid,pinSubType,infos);}
 int32_t DeleteUser(int32_t userId, const std::vector<uint8_t>& authToken, std::vector<OHOS::HDI::UserAuth::V4_0::CredentialInfo>& deletedInfos, std::vector<uint8_t>& rootSecret) override {return base_->DeleteUser(userId,authToken,deletedInfos,rootSecret);}
 int32_t EnforceDeleteUser(int32_t userId, std::vector<OHOS::HDI::UserAuth::V4_0::CredentialInfo>& deletedInfos) override {return base_->EnforceDeleteUser(userId,deletedInfos);}
 int32_t UpdateAuthenticationResult(uint64_t contextId, const std::vector<uint8_t>& scheduleResult, OHOS::HDI::UserAuth::V4_0::AuthResultInfo& info, OHOS::HDI::UserAuth::V4_0::EnrolledState& enrolledState) override {return base_->UpdateAuthenticationResult(contextId,scheduleResult,info,enrolledState);}
 int32_t CancelAuthentication(uint64_t contextId) override {return base_->CancelAuthentication(contextId);}
 int32_t UpdateIdentificationResult(uint64_t contextId, const std::vector<uint8_t>& scheduleResult, OHOS::HDI::UserAuth::V4_0::IdentifyResultInfo& info) override {return base_->UpdateIdentificationResult(contextId,scheduleResult,info);}
 int32_t CancelIdentification(uint64_t contextId) override {return base_->CancelIdentification(contextId);}
 int32_t GetAvailableStatus(int32_t userId, int32_t authType, uint32_t authTrustLevel, int32_t& checkResult) override {return base_->GetAvailableStatus(userId,authType,authTrustLevel,checkResult);}
 int32_t GetValidSolution(int32_t userId, const std::vector<int32_t>& authTypes, uint32_t authTrustLevel, std::vector<int32_t>& validTypes) override {return base_->GetValidSolution(userId,authTypes,authTrustLevel,validTypes);}
 int32_t BeginAuthentication(uint64_t contextId, const OHOS::HDI::UserAuth::V4_0::AuthParam& param, std::vector<OHOS::HDI::UserAuth::V4_0::ScheduleInfo>& scheduleInfos) override {return base_->BeginAuthentication(contextId,param,scheduleInfos);}
 int32_t BeginEnrollment(const std::vector<uint8_t>& authToken, const OHOS::HDI::UserAuth::V4_0::EnrollParam& param, OHOS::HDI::UserAuth::V4_0::ScheduleInfo& info) override {return base_->BeginEnrollment(authToken,param,info);}
 int32_t BeginIdentification(uint64_t contextId, int32_t authType, const std::vector<uint8_t>& challenge, uint32_t executorSensorHint, OHOS::HDI::UserAuth::V4_0::ScheduleInfo& scheduleInfo) override {return base_->BeginIdentification(contextId,authType,challenge,executorSensorHint,scheduleInfo);}
 int32_t GetAllUserInfo(std::vector<OHOS::HDI::UserAuth::V4_0::UserInfo>& userInfos) override {return base_->GetAllUserInfo(userInfos);}
 int32_t GetAllExtUserInfo(std::vector<OHOS::HDI::UserAuth::V4_0::ExtUserInfo>& userInfos) override {return base_->GetAllExtUserInfo(userInfos);}
 int32_t GetEnrolledState(int32_t userId, int32_t authType, OHOS::HDI::UserAuth::V4_0::EnrolledState& enrolledState) override {return base_->GetEnrolledState(userId,authType,enrolledState);}
 int32_t CheckReuseUnlockResult(const OHOS::HDI::UserAuth::V4_0::ReuseUnlockParam& reuseParam, OHOS::HDI::UserAuth::V4_0::ReuseUnlockInfo& reuseInfo) override {return base_->CheckReuseUnlockResult(reuseParam,reuseInfo);}
 int32_t SendMessage(uint64_t scheduleId, int32_t srcRole, const std::vector<uint8_t>& msg) override {return base_->SendMessage(scheduleId,srcRole,msg);}
 int32_t RegisterMessageCallback(const sptr<OHOS::HDI::UserAuth::V4_0::IMessageCallback>& messageCallback) override {return base_->RegisterMessageCallback(messageCallback);}
 int32_t PrepareRemoteAuth(const std::string& remoteUdid) override {return base_->PrepareRemoteAuth(remoteUdid);}
 int32_t GetLocalScheduleFromMessage(const std::string& remoteUdid, const std::vector<uint8_t>& message, OHOS::HDI::UserAuth::V4_0::ScheduleInfo& scheduleInfo) override {return base_->GetLocalScheduleFromMessage(remoteUdid,message,scheduleInfo);}
 int32_t GetSignedExecutorInfo(const std::vector<int32_t>& authTypes, int32_t executorRole, const std::string& remoteUdid, std::vector<uint8_t>& signedExecutorInfo) override {return base_->GetSignedExecutorInfo(authTypes,executorRole,remoteUdid,signedExecutorInfo);}
 int32_t GetAuthResultFromMessage(const std::string& remoteUdid, const std::vector<uint8_t>& message, OHOS::HDI::UserAuth::V4_0::AuthResultInfo& authResultInfo) override {return base_->GetAuthResultFromMessage(remoteUdid,message,authResultInfo);}
 int32_t SetGlobalConfigParam(const OHOS::HDI::UserAuth::V4_0::GlobalConfigParam& param) override {return base_->SetGlobalConfigParam(param);}
 int32_t GetCredentialById(uint64_t credentialId, OHOS::HDI::UserAuth::V4_0::CredentialInfo& info) override {return base_->GetCredentialById(credentialId,info);}
 int32_t VerifyAuthToken(const std::vector<uint8_t>& tokenIn, uint64_t allowableDuration, OHOS::HDI::UserAuth::V4_0::UserAuthTokenPlain& tokenPlainOut, std::vector<uint8_t>& rootSecret) override {return base_->VerifyAuthToken(tokenIn,allowableDuration,tokenPlainOut,rootSecret);}
 int32_t UpdateAbandonResult(int32_t userId, const std::vector<uint8_t>& scheduleResult, std::vector<OHOS::HDI::UserAuth::V4_0::CredentialInfo>& infos) override {return base_->UpdateAbandonResult(userId,scheduleResult,infos);}
 int32_t ClearUnavailableCredential(const std::vector<int32_t>& userIds, std::vector<OHOS::HDI::UserAuth::V4_0::CredentialInfo>& infos) override {return base_->ClearUnavailableCredential(userIds,infos);}
};
extern "C" HDI::UserAuth::V4_1::IUserAuthInterface *UserAuthInterfaceImplGetInstance(){
 static void *lib=dlopen("/compat/lib64/libdipper_userauth_4_0.so",RTLD_NOW|RTLD_LOCAL);
 if(!lib)return nullptr;
 auto factory=reinterpret_cast<Base*(*)()>(dlsym(lib,"UserAuthInterfaceImplGetInstance"));
 if(!factory)return nullptr;
 Base *base=factory();if(!base)return nullptr;
 return new(std::nothrow) DipperUserAuth41(base);
}
