#include "pushservice.h"
#include "bfrobot.grpc.pb.h"
#include "grpc++/grpc++.h"
#include "servicemgr.h"
#include <QThread>
#include <atomic>

using namespace bftrader;
using namespace bftrader::bfrobot;

//
// RobotClient，实现异步客户端，不需要等客户端的应答就直接返回，避免一个客户端堵住其他的=
//
class IGrpcCb {
public:
    explicit IGrpcCb(QString clientId)
    {
        std::chrono::system_clock::time_point deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(deadline_);
        context_.set_deadline(deadline);
        context_.AddMetadata("clientid", clientId.toStdString());
    }
    virtual ~IGrpcCb()
    {
    }

    grpc::ClientContext& context() { return context_; }
    grpc::Status& status() { return status_; }

public:
    virtual void operator()() {}

protected:
    grpc::ClientContext context_;
    grpc::Status status_;
    const int deadline_ = 500;
};

template <class Resp>
class GrpcCb : public IGrpcCb {
public:
    explicit GrpcCb(QString clientId)
        : IGrpcCb(clientId)
    {
    }
    virtual ~GrpcCb() override {}

public:
    typedef std::unique_ptr<grpc::ClientAsyncResponseReader<Resp> > RpcPtr;

public:
    Resp& getResp() { return resp_; }
    void setRpcPtrAndFinish(RpcPtr rpc)
    {
        rpc_.swap(rpc);
        rpc_->Finish(&resp_, &status_, (void*)this);
    }

public:
    virtual void operator()() override
    {
    }

private:
    RpcPtr rpc_;
    Resp resp_;
};

class RobotClient;
class PingCb final : public GrpcCb<BfPingData> {
public:
    explicit PingCb(RobotClient* robotClient, QString clientId)
        : GrpcCb<BfPingData>(clientId)
        , robotClient_(robotClient)
    {
    }
    virtual ~PingCb() override {}

public:
    virtual void operator()() override;

private:
    RobotClient* robotClient_ = nullptr;
};

class RobotClient {
public:
    RobotClient(std::shared_ptr<grpc::Channel> channel, QString ctaId, const BfConnectReq& req)
        : stub_(BfRobotService::NewStub(channel))
        , ctaId_(ctaId)
        , req_(req)
    {
        BfDebug(__FUNCTION__);
        cq_thread_ = new QThread();
        std::function<void(void)> fn = [=]() {
            for (;;) {
                void* pTag;
                bool ok = false;
                bool result = this->cq_.Next(&pTag, &ok);
                if (result) {
                    std::unique_ptr<IGrpcCb> pCb(static_cast<IGrpcCb*>(pTag));
                    // run callback
                    (*pCb)();
                } else {
                    // shutdown
                    BfDebug("cq_thread shutdown");
                    break;
                }
            }
        };
        QObject::connect(cq_thread_, &QThread::started, fn);
        cq_thread_->start();
    }
    ~RobotClient()
    {
        BfDebug(__FUNCTION__);
        cq_.Shutdown();
        cq_thread_->quit();
        cq_thread_->wait();
        delete cq_thread_;
        cq_thread_ = nullptr;
    }
    void OnPing(const BfPingData& data)
    {
        auto pCb = new PingCb(this, this->ctaId());
        pCb->setRpcPtrAndFinish(stub_->AsyncOnPing(&pCb->context(), data, &cq_));
    }
    void OnTick(const BfTickData& data)
    {
        auto pCb = new GrpcCb<BfVoid>(this->ctaId());
        pCb->setRpcPtrAndFinish(stub_->AsyncOnTick(&pCb->context(), data, &cq_));
    }
    void OnTrade(const BfTradeData& data)
    {
        auto pCb = new GrpcCb<BfVoid>(this->ctaId());
        pCb->setRpcPtrAndFinish(stub_->AsyncOnTrade(&pCb->context(), data, &cq_));
    }
    void OnOrder(const BfOrderData& data)
    {
        auto pCb = new GrpcCb<BfVoid>(this->ctaId());
        pCb->setRpcPtrAndFinish(stub_->AsyncOnOrder(&pCb->context(), data, &cq_));
    }
    void OnInit(const BfVoid& data)
    {
        auto pCb = new GrpcCb<BfVoid>(this->ctaId());
        pCb->setRpcPtrAndFinish(stub_->AsyncOnInit(&pCb->context(), data, &cq_));
    }
    void OnStart(const BfVoid& data)
    {
        auto pCb = new GrpcCb<BfVoid>(this->ctaId());
        pCb->setRpcPtrAndFinish(stub_->AsyncOnStart(&pCb->context(), data, &cq_));
    }
    void OnStop(const BfVoid& data)
    {
        auto pCb = new GrpcCb<BfVoid>(this->ctaId());
        pCb->setRpcPtrAndFinish(stub_->AsyncOnStop(&pCb->context(), data, &cq_));
    }

public:
    bool logHandler() { return req_.loghandler(); }
    bool tickHandler() { return req_.tickhandler(); }
    bool tradehandler() { return req_.tradehandler(); }
    bool subscribled(const std::string& symbol, const std::string& exchange)
    {
        if (req_.symbol() == "*") {
            return true;
        }
        if (symbol == req_.symbol()) {
            return true;
        }
        return false;
    }
    void incPingFailCount() { pingfail_count_++; }
    int pingFailCount() { return pingfail_count_; }
    void resetPingFailCount() { pingfail_count_ = 0; }
    QString ctaId() { return ctaId_; }
    QString robotId() { return req_.clientid().c_str(); }

private:
    std::unique_ptr<BfRobotService::Stub> stub_;
    std::atomic_int32_t pingfail_count_ = 0;
    const int deadline_ = 500;
    QString ctaId_;
    BfConnectReq req_;

    // async client
    grpc::CompletionQueue cq_;
    QThread* cq_thread_ = nullptr;
};

void PingCb::operator()()
{
    if (!status_.ok()) {
        QString robotId = robotClient_->robotId();
        robotClient_->incPingFailCount();
        int failCount = robotClient_->pingFailCount();
        int errorCode = status_.error_code();
        std::string errorMsg = status_.error_message();
        BfError("(%s)->OnPing(%dms) fail(%d),code:%d,msg:%s", qPrintable(robotId), deadline_, failCount, errorCode, errorMsg.c_str());
        //if (failCount > 3) {
        //    BfError("(%s)->OnPing fail too mang times,so kill it", qPrintable(robotId));
        //    QMetaObject::invokeMethod(g_sm->pushService(), "disconnectRobot", Qt::QueuedConnection, Q_ARG(QString, robotId));
        //}
        return;
    }
    robotClient_->resetPingFailCount();
}

//
// PushService
//
PushService::PushService(QObject* parent)
    : QObject(parent)
{
}

void PushService::init()
{
    BfDebug(__FUNCTION__);
    g_sm->checkCurrentOn(ServiceMgr::PUSH);

    // start timer
    this->pingTimer_ = new QTimer(this);
    this->pingTimer_->setInterval(5 * 1000);
    QObject::connect(this->pingTimer_, &QTimer::timeout, this, &PushService::onPing);
    this->pingTimer_->start();
}

void PushService::shutdown()
{
    BfDebug(__FUNCTION__);
    g_sm->checkCurrentOn(ServiceMgr::PUSH);

    // close timer
    this->pingTimer_->stop();
    delete this->pingTimer_;
    this->pingTimer_ = nullptr;

    // delete all robotclient
    for (auto client : clients_) {
        delete client;
    }
    clients_.clear();
}

void PushService::connectRobot(QString ctaId, const BfConnectReq& req)
{
    BfDebug(__FUNCTION__);
    g_sm->checkCurrentOn(ServiceMgr::PUSH);

    QString endpoint = QString().sprintf("%s:%d", req.clientip().c_str(), req.clientport());
    QString robotId = req.clientid().c_str();

    RobotClient* client = new RobotClient(grpc::CreateChannel(endpoint.toStdString(), grpc::InsecureChannelCredentials()),
        ctaId, req);

    if (clients_.contains(robotId)) {
        auto it = clients_[robotId];
        delete it;
        clients_.remove(robotId);
    }
    clients_[robotId] = client;
}

void PushService::disconnectRobot(QString robotId)
{
    BfDebug(__FUNCTION__);
    g_sm->checkCurrentOn(ServiceMgr::PUSH);

    if (clients_.contains(robotId)) {
        BfDebug("delete robotclient:%s", qPrintable(robotId));
        RobotClient* client = clients_[robotId];
        delete client;
        clients_.remove(robotId);
    }
}

void PushService::onCtaClosed()
{
    BfDebug(__FUNCTION__);
    g_sm->checkCurrentOn(ServiceMgr::PUSH);

    for (auto client : clients_) {
        delete client;
    }
    clients_.clear();
}

void PushService::onPing()
{
    g_sm->checkCurrentOn(ServiceMgr::PUSH);

    BfPingData data;
    data.set_message("cta");
    for (auto client : clients_) {
        client->OnPing(data);
    }
}

void PushService::onGotTick(const BfTickData& bfItem)
{

}

void PushService::onGotTrade(const BfTradeData& bfItem)
{

}

void PushService::onGotOrder(const BfOrderData& bfItem)
{

}

void PushService::onAutoTradingStart()
{

}

void PushService::onAutoTradingStop()
{

}