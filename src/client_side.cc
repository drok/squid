/*
 * Copyright (C) 1996-2015 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 33    Client-side Routines */

/**
 \defgroup ClientSide Client-Side Logics
 *
 \section cserrors Errors and client side
 *
 \par Problem the first:
 * the store entry is no longer authoritative on the
 * reply status. EBITTEST (E_ABORT) is no longer a valid test outside
 * of client_side_reply.c.
 * Problem the second: resources are wasted if we delay in cleaning up.
 * Problem the third we can't depend on a connection close to clean up.
 *
 \par Nice thing the first:
 * Any step in the stream can callback with data
 * representing an error.
 * Nice thing the second: once you stop requesting reads from upstream,
 * upstream can be stopped too.
 *
 \par Solution #1:
 * Error has a callback mechanism to hand over a membuf
 * with the error content. The failing node pushes that back as the
 * reply. Can this be generalised to reduce duplicate efforts?
 * A: Possibly. For now, only one location uses this.
 * How to deal with pre-stream errors?
 * Tell client_side_reply that we *want* an error page before any
 * stream calls occur. Then we simply read as normal.
 *
 *
 \section pconn_logic Persistent connection logic:
 *
 \par
 * requests (httpClientRequest structs) get added to the connection
 * list, with the current one being chr
 *
 \par
 * The request is *immediately* kicked off, and data flows through
 * to clientSocketRecipient.
 *
 \par
 * If the data that arrives at clientSocketRecipient is not for the current
 * request, clientSocketRecipient simply returns, without requesting more
 * data, or sending it.
 *
 \par
 * ClientKeepAliveNextRequest will then detect the presence of data in
 * the next ClientHttpRequest, and will send it, restablishing the
 * data flow.
 */

#include "squid.h"
#include "acl/FilledChecklist.h"
#include "anyp/PortCfg.h"
#include "base/Subscription.h"
#include "base/TextException.h"
#include "CachePeer.h"
#include "ChunkedCodingParser.h"
#include "client_db.h"
#include "client_side.h"
#include "client_side_reply.h"
#include "client_side_request.h"
#include "ClientRequestContext.h"
#include "clientStream.h"
#include "comm.h"
#include "comm/Connection.h"
#include "comm/Loops.h"
#include "comm/Read.h"
#include "comm/TcpAcceptor.h"
#include "comm/Write.h"
#include "CommCalls.h"
#include "errorpage.h"
#include "fd.h"
#include "fde.h"
#include "fqdncache.h"
#include "FwdState.h"
#include "globals.h"
#include "helper.h"
#include "helper/Reply.h"
#include "http.h"
#include "http/one/RequestParser.h"
#include "HttpHdrContRange.h"
#include "HttpHeaderTools.h"
#include "HttpReply.h"
#include "HttpRequest.h"
#include "ident/Config.h"
#include "ident/Ident.h"
#include "internal.h"
#include "ipc/FdNotes.h"
#include "ipc/StartListening.h"
#include "log/access_log.h"
#include "MemBuf.h"
#include "MemObject.h"
#include "mime_header.h"
#include "parser/Tokenizer.h"
#include "profiler/Profiler.h"
#include "rfc1738.h"
#include "servers/forward.h"
#include "SquidConfig.h"
#include "SquidTime.h"
#include "StatCounters.h"
#include "StatHist.h"
#include "Store.h"
#include "TimeOrTag.h"
#include "tools.h"
#include "URL.h"

#if USE_AUTH
#include "auth/UserRequest.h"
#endif
#if USE_DELAY_POOLS
#include "ClientInfo.h"
#endif
#if USE_OPENSSL
#include "ssl/bio.h"
#include "ssl/context_storage.h"
#include "ssl/gadgets.h"
#include "ssl/helper.h"
#include "ssl/ProxyCerts.h"
#include "ssl/ServerBump.h"
#include "ssl/support.h"
#endif
#if USE_SSL_CRTD
#include "ssl/certificate_db.h"
#include "ssl/crtd_message.h"
#endif

// for tvSubUsec() which should be in SquidTime.h
#include "util.h"

#include <climits>
#include <cmath>
#include <limits>

#if LINGERING_CLOSE
#define comm_close comm_lingering_close
#endif

/// dials clientListenerConnectionOpened call
class ListeningStartedDialer: public CallDialer, public Ipc::StartListeningCb
{
public:
    typedef void (*Handler)(AnyP::PortCfgPointer &portCfg, const Ipc::FdNoteId note, const Subscription::Pointer &sub);
    ListeningStartedDialer(Handler aHandler, AnyP::PortCfgPointer &aPortCfg, const Ipc::FdNoteId note, const Subscription::Pointer &aSub):
        handler(aHandler), portCfg(aPortCfg), portTypeNote(note), sub(aSub) {}

    virtual void print(std::ostream &os) const {
        startPrint(os) <<
                       ", " << FdNote(portTypeNote) << " port=" << (void*)&portCfg << ')';
    }

    virtual bool canDial(AsyncCall &) const { return true; }
    virtual void dial(AsyncCall &) { (handler)(portCfg, portTypeNote, sub); }

public:
    Handler handler;

private:
    AnyP::PortCfgPointer portCfg;   ///< from HttpPortList
    Ipc::FdNoteId portTypeNote;    ///< Type of IPC socket being opened
    Subscription::Pointer sub; ///< The handler to be subscribed for this connetion listener
};

static void clientListenerConnectionOpened(AnyP::PortCfgPointer &s, const Ipc::FdNoteId portTypeNote, const Subscription::Pointer &sub);

/* our socket-related context */

CBDATA_CLASS_INIT(ClientSocketContext);

/* Local functions */
static IOCB clientWriteComplete;
static IOCB clientWriteBodyComplete;
static IOACB httpAccept;
#if USE_OPENSSL
static IOACB httpsAccept;
#endif
static CTCB clientLifetimeTimeout;
#if USE_IDENT
static IDCB clientIdentDone;
#endif
static int clientIsContentLengthValid(HttpRequest * r);
static int clientIsRequestBodyTooLargeForPolicy(int64_t bodyLength);

static void clientUpdateStatHistCounters(LogTags logType, int svc_time);
static void clientUpdateStatCounters(LogTags logType);
static void clientUpdateHierCounters(HierarchyLogEntry *);
static bool clientPingHasFinished(ping_data const *aPing);
void prepareLogWithRequestDetails(HttpRequest *, AccessLogEntry::Pointer &);
#ifndef PURIFY
static bool connIsUsable(ConnStateData * conn);
#endif
static void ClientSocketContextPushDeferredIfNeeded(ClientSocketContext::Pointer deferredRequest, ConnStateData * conn);
static void clientUpdateSocketStats(LogTags logType, size_t size);

char *skipLeadingSpace(char *aString);

clientStreamNode *
ClientSocketContext::getTail() const
{
    if (http->client_stream.tail)
        return (clientStreamNode *)http->client_stream.tail->data;

    return NULL;
}

clientStreamNode *
ClientSocketContext::getClientReplyContext() const
{
    return (clientStreamNode *)http->client_stream.tail->prev->data;
}

ConnStateData *
ClientSocketContext::getConn() const
{
    return http->getConn();
}

/**
 * This routine should be called to grow the in.buf and then
 * call Comm::Read().
 */
void
ConnStateData::readSomeData()
{
    if (reading())
        return;

    debugs(33, 4, HERE << clientConnection << ": reading request...");

    // we can only read if there is more than 1 byte of space free
    if (Config.maxRequestBufferSize - in.buf.length() < 2)
        return;

    typedef CommCbMemFunT<ConnStateData, CommIoCbParams> Dialer;
    reader = JobCallback(33, 5, Dialer, this, ConnStateData::clientReadRequest);
    Comm::Read(clientConnection, reader);
}

void
ClientSocketContext::removeFromConnectionList(ConnStateData * conn)
{
    ClientSocketContext::Pointer *tempContextPointer;
    assert(conn != NULL && cbdataReferenceValid(conn));
    assert(conn->getCurrentContext() != NULL);
    /* Unlink us from the connection request list */
    tempContextPointer = & conn->currentobject;

    while (tempContextPointer->getRaw()) {
        if (*tempContextPointer == this)
            break;

        tempContextPointer = &(*tempContextPointer)->next;
    }

    assert(tempContextPointer->getRaw() != NULL);
    *tempContextPointer = next;
    next = NULL;
}

ClientSocketContext::~ClientSocketContext()
{
    clientStreamNode *node = getTail();

    if (node) {
        ClientSocketContext *streamContext = dynamic_cast<ClientSocketContext *> (node->data.getRaw());

        if (streamContext) {
            /* We are *always* the tail - prevent recursive free */
            assert(this == streamContext);
            node->data = NULL;
        }
    }

    if (connRegistered_)
        deRegisterWithConn();

    httpRequestFree(http);

    /* clean up connection links to us */
    assert(this != next.getRaw());
}

void
ClientSocketContext::registerWithConn()
{
    assert (!connRegistered_);
    assert (http);
    assert (http->getConn() != NULL);
    connRegistered_ = true;
    http->getConn()->addContextToQueue(this);
}

void
ClientSocketContext::deRegisterWithConn()
{
    assert (connRegistered_);
    removeFromConnectionList(http->getConn());
    connRegistered_ = false;
}

void
ClientSocketContext::connIsFinished()
{
    assert (http);
    assert (http->getConn() != NULL);
    deRegisterWithConn();
    /* we can't handle any more stream data - detach */
    clientStreamDetach(getTail(), http);
}

ClientSocketContext::ClientSocketContext(const Comm::ConnectionPointer &aConn, ClientHttpRequest *aReq) :
    clientConnection(aConn),
    http(aReq),
    reply(NULL),
    next(NULL),
    writtenToSocket(0),
    mayUseConnection_ (false),
    connRegistered_ (false)
{
    assert(http != NULL);
    memset (reqbuf, '\0', sizeof (reqbuf));
    flags.deferred = 0;
    flags.parsed_ok = 0;
    deferredparams.node = NULL;
    deferredparams.rep = NULL;
}

void
ClientSocketContext::writeControlMsg(HttpControlMsg &msg)
{
    HttpReply::Pointer rep(msg.reply);
    Must(rep != NULL);

    // remember the callback
    cbControlMsgSent = msg.cbSuccess;

    AsyncCall::Pointer call = commCbCall(33, 5, "ClientSocketContext::wroteControlMsg",
                                         CommIoCbPtrFun(&WroteControlMsg, this));

    getConn()->writeControlMsgAndCall(this, rep.getRaw(), call);
}

/// called when we wrote the 1xx response
void
ClientSocketContext::wroteControlMsg(const Comm::ConnectionPointer &conn, char *, size_t, Comm::Flag errflag, int xerrno)
{
    if (errflag == Comm::ERR_CLOSING)
        return;

    if (errflag == Comm::OK) {
        ScheduleCallHere(cbControlMsgSent);
        return;
    }

    debugs(33, 3, HERE << "1xx writing failed: " << xstrerr(xerrno));
    // no error notification: see HttpControlMsg.h for rationale and
    // note that some errors are detected elsewhere (e.g., close handler)

    // close on 1xx errors to be conservative and to simplify the code
    // (if we do not close, we must notify the source of a failure!)
    conn->close();

    // XXX: writeControlMsgAndCall() should handle writer-specific writing
    // results, including errors and then call us with success/failure outcome.
}

/// wroteControlMsg() wrapper: ClientSocketContext is not an AsyncJob
void
ClientSocketContext::WroteControlMsg(const Comm::ConnectionPointer &conn, char *bufnotused, size_t size, Comm::Flag errflag, int xerrno, void *data)
{
    ClientSocketContext *context = static_cast<ClientSocketContext*>(data);
    context->wroteControlMsg(conn, bufnotused, size, errflag, xerrno);
}

#if USE_IDENT
static void
clientIdentDone(const char *ident, void *data)
{
    ConnStateData *conn = (ConnStateData *)data;
    xstrncpy(conn->clientConnection->rfc931, ident ? ident : dash_str, USER_IDENT_SZ);
}
#endif

void
clientUpdateStatCounters(LogTags logType)
{
    ++statCounter.client_http.requests;

    if (logTypeIsATcpHit(logType))
        ++statCounter.client_http.hits;

    if (logType == LOG_TCP_HIT)
        ++statCounter.client_http.disk_hits;
    else if (logType == LOG_TCP_MEM_HIT)
        ++statCounter.client_http.mem_hits;
}

void
clientUpdateStatHistCounters(LogTags logType, int svc_time)
{
    statCounter.client_http.allSvcTime.count(svc_time);
    /**
     * The idea here is not to be complete, but to get service times
     * for only well-defined types.  For example, we don't include
     * LOG_TCP_REFRESH_FAIL because its not really a cache hit
     * (we *tried* to validate it, but failed).
     */

    switch (logType) {

    case LOG_TCP_REFRESH_UNMODIFIED:
        statCounter.client_http.nearHitSvcTime.count(svc_time);
        break;

    case LOG_TCP_IMS_HIT:
        statCounter.client_http.nearMissSvcTime.count(svc_time);
        break;

    case LOG_TCP_HIT:

    case LOG_TCP_MEM_HIT:

    case LOG_TCP_OFFLINE_HIT:
        statCounter.client_http.hitSvcTime.count(svc_time);
        break;

    case LOG_TCP_MISS:

    case LOG_TCP_CLIENT_REFRESH_MISS:
        statCounter.client_http.missSvcTime.count(svc_time);
        break;

    default:
        /* make compiler warnings go away */
        break;
    }
}

bool
clientPingHasFinished(ping_data const *aPing)
{
    if (0 != aPing->stop.tv_sec && 0 != aPing->start.tv_sec)
        return true;

    return false;
}

void
clientUpdateHierCounters(HierarchyLogEntry * someEntry)
{
    ping_data *i;

    switch (someEntry->code) {
#if USE_CACHE_DIGESTS

    case CD_PARENT_HIT:

    case CD_SIBLING_HIT:
        ++ statCounter.cd.times_used;
        break;
#endif

    case SIBLING_HIT:

    case PARENT_HIT:

    case FIRST_PARENT_MISS:

    case CLOSEST_PARENT_MISS:
        ++ statCounter.icp.times_used;
        i = &someEntry->ping;

        if (clientPingHasFinished(i))
            statCounter.icp.querySvcTime.count(tvSubUsec(i->start, i->stop));

        if (i->timeout)
            ++ statCounter.icp.query_timeouts;

        break;

    case CLOSEST_PARENT:

    case CLOSEST_DIRECT:
        ++ statCounter.netdb.times_used;

        break;

    default:
        break;
    }
}

void
ClientHttpRequest::updateCounters()
{
    clientUpdateStatCounters(logType);

    if (request->errType != ERR_NONE)
        ++ statCounter.client_http.errors;

    clientUpdateStatHistCounters(logType,
                                 tvSubMsec(al->cache.start_time, current_time));

    clientUpdateHierCounters(&request->hier);
}

void
prepareLogWithRequestDetails(HttpRequest * request, AccessLogEntry::Pointer &aLogEntry)
{
    assert(request);
    assert(aLogEntry != NULL);

    if (Config.onoff.log_mime_hdrs) {
        Packer p;
        MemBuf mb;
        mb.init();
        packerToMemInit(&p, &mb);
        request->header.packInto(&p);
        //This is the request after adaptation or redirection
        aLogEntry->headers.adapted_request = xstrdup(mb.buf);

        // the virgin request is saved to aLogEntry->request
        if (aLogEntry->request) {
            packerClean(&p);
            mb.reset();
            packerToMemInit(&p, &mb);
            aLogEntry->request->header.packInto(&p);
            aLogEntry->headers.request = xstrdup(mb.buf);
        }

#if USE_ADAPTATION
        const Adaptation::History::Pointer ah = request->adaptLogHistory();
        if (ah != NULL) {
            packerClean(&p);
            mb.reset();
            packerToMemInit(&p, &mb);
            ah->lastMeta.packInto(&p);
            aLogEntry->adapt.last_meta = xstrdup(mb.buf);
        }
#endif

        packerClean(&p);
        mb.clean();
    }

#if ICAP_CLIENT
    const Adaptation::Icap::History::Pointer ih = request->icapHistory();
    if (ih != NULL)
        ih->processingTime(aLogEntry->icap.processingTime);
#endif

    aLogEntry->http.method = request->method;
    aLogEntry->http.version = request->http_ver;
    aLogEntry->hier = request->hier;
    if (request->content_length > 0) // negative when no body or unknown length
        aLogEntry->http.clientRequestSz.payloadData += request->content_length; // XXX: actually adaptedRequest payload size ??
    aLogEntry->cache.extuser = request->extacl_user.termedBuf();

    // Adapted request, if any, inherits and then collects all the stats, but
    // the virgin request gets logged instead; copy the stats to log them.
    // TODO: avoid losses by keeping these stats in a shared history object?
    if (aLogEntry->request) {
        aLogEntry->request->dnsWait = request->dnsWait;
        aLogEntry->request->errType = request->errType;
        aLogEntry->request->errDetail = request->errDetail;
    }
}

void
ClientHttpRequest::logRequest()
{
    if (!out.size && !logType)
        debugs(33, 5, HERE << "logging half-baked transaction: " << log_uri);

    al->icp.opcode = ICP_INVALID;
    al->url = log_uri;
    debugs(33, 9, "clientLogRequest: al.url='" << al->url << "'");

    if (al->reply) {
        al->http.code = al->reply->sline.status();
        al->http.content_type = al->reply->content_type.termedBuf();
    } else if (loggingEntry() && loggingEntry()->mem_obj) {
        al->http.code = loggingEntry()->mem_obj->getReply()->sline.status();
        al->http.content_type = loggingEntry()->mem_obj->getReply()->content_type.termedBuf();
    }

    debugs(33, 9, "clientLogRequest: http.code='" << al->http.code << "'");

    if (loggingEntry() && loggingEntry()->mem_obj && loggingEntry()->objectLen() >= 0)
        al->cache.objectSize = loggingEntry()->contentLen(); // payload duplicate ?? with or without TE ?

    al->http.clientRequestSz.header = req_sz;
    al->http.clientReplySz.header = out.headers_sz;
    // XXX: calculate without payload encoding or headers !!
    al->http.clientReplySz.payloadData = out.size - out.headers_sz; // pretend its all un-encoded data for now.

    al->cache.highOffset = out.offset;

    al->cache.code = logType;

    tvSub(al->cache.trTime, al->cache.start_time, current_time);

    if (request)
        prepareLogWithRequestDetails(request, al);

    if (getConn() != NULL && getConn()->clientConnection != NULL && getConn()->clientConnection->rfc931[0])
        al->cache.rfc931 = getConn()->clientConnection->rfc931;

#if USE_OPENSSL && 0

    /* This is broken. Fails if the connection has been closed. Needs
     * to snarf the ssl details some place earlier..
     */
    if (getConn() != NULL)
        al->cache.ssluser = sslGetUserEmail(fd_table[getConn()->fd].ssl);

#endif

    /* Add notes (if we have a request to annotate) */
    if (request) {
        // The al->notes and request->notes must point to the same object.
        (void)SyncNotes(*al, *request);
        for (auto i = Config.notes.begin(); i != Config.notes.end(); ++i) {
            if (const char *value = (*i)->match(request, al->reply, NULL)) {
                NotePairs &notes = SyncNotes(*al, *request);
                notes.add((*i)->key.termedBuf(), value);
                debugs(33, 3, (*i)->key.termedBuf() << " " << value);
            }
        }
    }

    ACLFilledChecklist checklist(NULL, request, NULL);
    if (al->reply) {
        checklist.reply = al->reply;
        HTTPMSGLOCK(checklist.reply);
    }

    if (request) {
        al->adapted_request = request;
        HTTPMSGLOCK(al->adapted_request);
    }
    accessLogLog(al, &checklist);

    bool updatePerformanceCounters = true;
    if (Config.accessList.stats_collection) {
        ACLFilledChecklist statsCheck(Config.accessList.stats_collection, request, NULL);
        if (al->reply) {
            statsCheck.reply = al->reply;
            HTTPMSGLOCK(statsCheck.reply);
        }
        updatePerformanceCounters = (statsCheck.fastCheck() == ACCESS_ALLOWED);
    }

    if (updatePerformanceCounters) {
        if (request)
            updateCounters();

        if (getConn() != NULL && getConn()->clientConnection != NULL)
            clientdbUpdate(getConn()->clientConnection->remote, logType, AnyP::PROTO_HTTP, out.size);
    }
}

void
ClientHttpRequest::freeResources()
{
    safe_free(uri);
    safe_free(log_uri);
    safe_free(redirect.location);
    range_iter.boundary.clean();
    HTTPMSGUNLOCK(request);

    if (client_stream.tail)
        clientStreamAbort((clientStreamNode *)client_stream.tail->data, this);
}

void
httpRequestFree(void *data)
{
    ClientHttpRequest *http = (ClientHttpRequest *)data;
    assert(http != NULL);
    delete http;
}

bool
ConnStateData::areAllContextsForThisConnection() const
{
    ClientSocketContext::Pointer context = getCurrentContext();

    while (context.getRaw()) {
        if (context->http->getConn() != this)
            return false;

        context = context->next;
    }

    return true;
}

void
ConnStateData::freeAllContexts()
{
    ClientSocketContext::Pointer context;

    while ((context = getCurrentContext()).getRaw() != NULL) {
        assert(getCurrentContext() !=
               getCurrentContext()->next);
        context->connIsFinished();
        assert (context != currentobject);
    }
}

/// propagates abort event to all contexts
void
ConnStateData::notifyAllContexts(int xerrno)
{
    typedef ClientSocketContext::Pointer CSCP;
    for (CSCP c = getCurrentContext(); c.getRaw(); c = c->next)
        c->noteIoError(xerrno);
}

/* This is a handler normally called by comm_close() */
void ConnStateData::connStateClosed(const CommCloseCbParams &)
{
    deleteThis("ConnStateData::connStateClosed");
}

#if USE_AUTH
void
ConnStateData::setAuth(const Auth::UserRequest::Pointer &aur, const char *by)
{
    if (auth_ == NULL) {
        if (aur != NULL) {
            debugs(33, 2, "Adding connection-auth to " << clientConnection << " from " << by);
            auth_ = aur;
        }
        return;
    }

    // clobered with self-pointer
    // NP: something nasty is going on in Squid, but harmless.
    if (aur == auth_) {
        debugs(33, 2, "WARNING: Ignoring duplicate connection-auth for " << clientConnection << " from " << by);
        return;
    }

    /*
     * Connection-auth relies on a single set of credentials being preserved
     * for all requests on a connection once they have been setup.
     * There are several things which need to happen to preserve security
     * when connection-auth credentials change unexpectedly or are unset.
     *
     * 1) auth helper released from any active state
     *
     * They can only be reserved by a handshake process which this
     * connection can now never complete.
     * This prevents helpers hanging when their connections close.
     *
     * 2) pinning is expected to be removed and server conn closed
     *
     * The upstream link is authenticated with the same credentials.
     * Expecting the same level of consistency we should have received.
     * This prevents upstream being faced with multiple or missing
     * credentials after authentication.
     * NP: un-pin is left to the cleanup in ConnStateData::swanSong()
     *     we just trigger that cleanup here via comm_reset_close() or
     *     ConnStateData::stopReceiving()
     *
     * 3) the connection needs to close.
     *
     * This prevents attackers injecting requests into a connection,
     * or gateways wrongly multiplexing users into a single connection.
     *
     * When credentials are missing closure needs to follow an auth
     * challenge for best recovery by the client.
     *
     * When credentials change there is nothing we can do but abort as
     * fast as possible. Sending TCP RST instead of an HTTP response
     * is the best-case action.
     */

    // clobbered with nul-pointer
    if (aur == NULL) {
        debugs(33, 2, "WARNING: Graceful closure on " << clientConnection << " due to connection-auth erase from " << by);
        auth_->releaseAuthServer();
        auth_ = NULL;
        // XXX: need to test whether the connection re-auth challenge is sent. If not, how to trigger it from here.
        // NP: the current situation seems to fix challenge loops in Safari without visible issues in others.
        // we stop receiving more traffic but can leave the Job running to terminate after the error or challenge is delivered.
        stopReceiving("connection-auth removed");
        return;
    }

    // clobbered with alternative credentials
    if (aur != auth_) {
        debugs(33, 2, "ERROR: Closing " << clientConnection << " due to change of connection-auth from " << by);
        auth_->releaseAuthServer();
        auth_ = NULL;
        // this is a fatal type of problem.
        // Close the connection immediately with TCP RST to abort all traffic flow
        comm_reset_close(clientConnection);
        return;
    }

    /* NOT REACHABLE */
}
#endif

// cleans up before destructor is called
void
ConnStateData::swanSong()
{
    debugs(33, 2, HERE << clientConnection);
    flags.readMore = false;
    clientdbEstablished(clientConnection->remote, -1);  /* decrement */
    assert(areAllContextsForThisConnection());
    freeAllContexts();

    unpinConnection(true);

    if (Comm::IsConnOpen(clientConnection))
        clientConnection->close();

#if USE_AUTH
    // NP: do this bit after closing the connections to avoid side effects from unwanted TCP RST
    setAuth(NULL, "ConnStateData::SwanSong cleanup");
#endif

    BodyProducer::swanSong();
    flags.swanSang = true;
}

bool
ConnStateData::isOpen() const
{
    return cbdataReferenceValid(this) && // XXX: checking "this" in a method
           Comm::IsConnOpen(clientConnection) &&
           !fd_table[clientConnection->fd].closing();
}

ConnStateData::~ConnStateData()
{
    debugs(33, 3, HERE << clientConnection);

    if (isOpen())
        debugs(33, DBG_IMPORTANT, "BUG: ConnStateData did not close " << clientConnection);

    if (!flags.swanSang)
        debugs(33, DBG_IMPORTANT, "BUG: ConnStateData was not destroyed properly; " << clientConnection);

    if (bodyPipe != NULL)
        stopProducingFor(bodyPipe, false);

#if USE_OPENSSL
    delete sslServerBump;
#endif
}

/**
 * clientSetKeepaliveFlag() sets request->flags.proxyKeepalive.
 * This is the client-side persistent connection flag.  We need
 * to set this relatively early in the request processing
 * to handle hacks for broken servers and clients.
 */
void
clientSetKeepaliveFlag(ClientHttpRequest * http)
{
    HttpRequest *request = http->request;

    debugs(33, 3, "http_ver = " << request->http_ver);
    debugs(33, 3, "method = " << request->method);

    // TODO: move to HttpRequest::hdrCacheInit, just like HttpReply.
    request->flags.proxyKeepalive = request->persistent();
}

static int
clientIsContentLengthValid(HttpRequest * r)
{
    switch (r->method.id()) {

    case Http::METHOD_GET:

    case Http::METHOD_HEAD:
        /* We do not want to see a request entity on GET/HEAD requests */
        return (r->content_length <= 0 || Config.onoff.request_entities);

    default:
        /* For other types of requests we don't care */
        return 1;
    }

    /* NOT REACHED */
}

int
clientIsRequestBodyTooLargeForPolicy(int64_t bodyLength)
{
    if (Config.maxRequestBodySize &&
            bodyLength > Config.maxRequestBodySize)
        return 1;       /* too large */

    return 0;
}

#ifndef PURIFY
bool
connIsUsable(ConnStateData * conn)
{
    if (conn == NULL || !cbdataReferenceValid(conn) || !Comm::IsConnOpen(conn->clientConnection))
        return false;

    return true;
}

#endif

// careful: the "current" context may be gone if we wrote an early response
ClientSocketContext::Pointer
ConnStateData::getCurrentContext() const
{
    return currentobject;
}

void
ClientSocketContext::deferRecipientForLater(clientStreamNode * node, HttpReply * rep, StoreIOBuffer receivedData)
{
    debugs(33, 2, "clientSocketRecipient: Deferring request " << http->uri);
    assert(flags.deferred == 0);
    flags.deferred = 1;
    deferredparams.node = node;
    deferredparams.rep = rep;
    deferredparams.queuedBuffer = receivedData;
    return;
}

bool
ClientSocketContext::startOfOutput() const
{
    return http->out.size == 0;
}

size_t
ClientSocketContext::lengthToSend(Range<int64_t> const &available)
{
    /*the size of available range can always fit in a size_t type*/
    size_t maximum = (size_t)available.size();

    if (!http->request->range)
        return maximum;

    assert (canPackMoreRanges());

    if (http->range_iter.debt() == -1)
        return maximum;

    assert (http->range_iter.debt() > 0);

    /* TODO this + the last line could be a range intersection calculation */
    if (available.start < http->range_iter.currentSpec()->offset)
        return 0;

    return min(http->range_iter.debt(), (int64_t)maximum);
}

void
ClientSocketContext::noteSentBodyBytes(size_t bytes)
{
    http->out.offset += bytes;

    if (!http->request->range)
        return;

    if (http->range_iter.debt() != -1) {
        http->range_iter.debt(http->range_iter.debt() - bytes);
        assert (http->range_iter.debt() >= 0);
    }

    /* debt() always stops at -1, below that is a bug */
    assert (http->range_iter.debt() >= -1);
}

bool
ClientHttpRequest::multipartRangeRequest() const
{
    return request->multipartRangeRequest();
}

bool
ClientSocketContext::multipartRangeRequest() const
{
    return http->multipartRangeRequest();
}

void
ClientSocketContext::sendBody(HttpReply * rep, StoreIOBuffer bodyData)
{
    assert(rep == NULL);

    if (!multipartRangeRequest() && !http->request->flags.chunkedReply) {
        size_t length = lengthToSend(bodyData.range());
        noteSentBodyBytes (length);
        AsyncCall::Pointer call = commCbCall(33, 5, "clientWriteBodyComplete",
                                             CommIoCbPtrFun(clientWriteBodyComplete, this));
        Comm::Write(clientConnection, bodyData.data, length, call, NULL);
        return;
    }

    MemBuf mb;
    mb.init();
    if (multipartRangeRequest())
        packRange(bodyData, &mb);
    else
        packChunk(bodyData, mb);

    if (mb.contentSize()) {
        /* write */
        AsyncCall::Pointer call = commCbCall(33, 5, "clientWriteComplete",
                                             CommIoCbPtrFun(clientWriteComplete, this));
        Comm::Write(clientConnection, &mb, call);
    }  else
        writeComplete(clientConnection, NULL, 0, Comm::OK);
}

/**
 * Packs bodyData into mb using chunked encoding. Packs the last-chunk
 * if bodyData is empty.
 */
void
ClientSocketContext::packChunk(const StoreIOBuffer &bodyData, MemBuf &mb)
{
    const uint64_t length =
        static_cast<uint64_t>(lengthToSend(bodyData.range()));
    noteSentBodyBytes(length);

    mb.Printf("%" PRIX64 "\r\n", length);
    mb.append(bodyData.data, length);
    mb.Printf("\r\n");
}

/** put terminating boundary for multiparts */
static void
clientPackTermBound(String boundary, MemBuf * mb)
{
    mb->Printf("\r\n--" SQUIDSTRINGPH "--\r\n", SQUIDSTRINGPRINT(boundary));
    debugs(33, 6, "clientPackTermBound: buf offset: " << mb->size);
}

/** appends a "part" HTTP header (as in a multi-part/range reply) to the buffer */
static void
clientPackRangeHdr(const HttpReply * rep, const HttpHdrRangeSpec * spec, String boundary, MemBuf * mb)
{
    HttpHeader hdr(hoReply);
    Packer p;
    assert(rep);
    assert(spec);

    /* put boundary */
    debugs(33, 5, "clientPackRangeHdr: appending boundary: " << boundary);
    /* rfc2046 requires to _prepend_ boundary with <crlf>! */
    mb->Printf("\r\n--" SQUIDSTRINGPH "\r\n", SQUIDSTRINGPRINT(boundary));

    /* stuff the header with required entries and pack it */

    if (rep->header.has(HDR_CONTENT_TYPE))
        hdr.putStr(HDR_CONTENT_TYPE, rep->header.getStr(HDR_CONTENT_TYPE));

    httpHeaderAddContRange(&hdr, *spec, rep->content_length);

    packerToMemInit(&p, mb);

    hdr.packInto(&p);

    packerClean(&p);

    hdr.clean();

    /* append <crlf> (we packed a header, not a reply) */
    mb->Printf("\r\n");
}

/**
 * extracts a "range" from *buf and appends them to mb, updating
 * all offsets and such.
 */
void
ClientSocketContext::packRange(StoreIOBuffer const &source, MemBuf * mb)
{
    HttpHdrRangeIter * i = &http->range_iter;
    Range<int64_t> available (source.range());
    char const *buf = source.data;

    while (i->currentSpec() && available.size()) {
        const size_t copy_sz = lengthToSend(available);

        if (copy_sz) {
            /*
             * intersection of "have" and "need" ranges must not be empty
             */
            assert(http->out.offset < i->currentSpec()->offset + i->currentSpec()->length);
            assert(http->out.offset + (int64_t)available.size() > i->currentSpec()->offset);

            /*
             * put boundary and headers at the beginning of a range in a
             * multi-range
             */

            if (http->multipartRangeRequest() && i->debt() == i->currentSpec()->length) {
                assert(http->memObject());
                clientPackRangeHdr(
                    http->memObject()->getReply(),  /* original reply */
                    i->currentSpec(),       /* current range */
                    i->boundary,    /* boundary, the same for all */
                    mb);
            }

            /*
             * append content
             */
            debugs(33, 3, "clientPackRange: appending " << copy_sz << " bytes");

            noteSentBodyBytes (copy_sz);

            mb->append(buf, copy_sz);

            /*
             * update offsets
             */
            available.start += copy_sz;

            buf += copy_sz;

        }

        if (!canPackMoreRanges()) {
            debugs(33, 3, "clientPackRange: Returning because !canPackMoreRanges.");

            if (i->debt() == 0)
                /* put terminating boundary for multiparts */
                clientPackTermBound(i->boundary, mb);

            return;
        }

        int64_t nextOffset = getNextRangeOffset();

        assert (nextOffset >= http->out.offset);

        int64_t skip = nextOffset - http->out.offset;

        /* adjust for not to be transmitted bytes */
        http->out.offset = nextOffset;

        if (available.size() <= (uint64_t)skip)
            return;

        available.start += skip;

        buf += skip;

        if (copy_sz == 0)
            return;
    }
}

/** returns expected content length for multi-range replies
 * note: assumes that httpHdrRangeCanonize has already been called
 * warning: assumes that HTTP headers for individual ranges at the
 *          time of the actuall assembly will be exactly the same as
 *          the headers when clientMRangeCLen() is called */
int
ClientHttpRequest::mRangeCLen()
{
    int64_t clen = 0;
    MemBuf mb;

    assert(memObject());

    mb.init();
    HttpHdrRange::iterator pos = request->range->begin();

    while (pos != request->range->end()) {
        /* account for headers for this range */
        mb.reset();
        clientPackRangeHdr(memObject()->getReply(),
                           *pos, range_iter.boundary, &mb);
        clen += mb.size;

        /* account for range content */
        clen += (*pos)->length;

        debugs(33, 6, "clientMRangeCLen: (clen += " << mb.size << " + " << (*pos)->length << ") == " << clen);
        ++pos;
    }

    /* account for the terminating boundary */
    mb.reset();

    clientPackTermBound(range_iter.boundary, &mb);

    clen += mb.size;

    mb.clean();

    return clen;
}

/**
 * returns true if If-Range specs match reply, false otherwise
 */
static int
clientIfRangeMatch(ClientHttpRequest * http, HttpReply * rep)
{
    const TimeOrTag spec = http->request->header.getTimeOrTag(HDR_IF_RANGE);
    /* check for parsing falure */

    if (!spec.valid)
        return 0;

    /* got an ETag? */
    if (spec.tag.str) {
        ETag rep_tag = rep->header.getETag(HDR_ETAG);
        debugs(33, 3, "clientIfRangeMatch: ETags: " << spec.tag.str << " and " <<
               (rep_tag.str ? rep_tag.str : "<none>"));

        if (!rep_tag.str)
            return 0;       /* entity has no etag to compare with! */

        if (spec.tag.weak || rep_tag.weak) {
            debugs(33, DBG_IMPORTANT, "clientIfRangeMatch: Weak ETags are not allowed in If-Range: " << spec.tag.str << " ? " << rep_tag.str);
            return 0;       /* must use strong validator for sub-range requests */
        }

        return etagIsStrongEqual(rep_tag, spec.tag);
    }

    /* got modification time? */
    if (spec.time >= 0) {
        return http->storeEntry()->lastmod <= spec.time;
    }

    assert(0);          /* should not happen */
    return 0;
}

/**
 * generates a "unique" boundary string for multipart responses
 * the caller is responsible for cleaning the string */
String
ClientHttpRequest::rangeBoundaryStr() const
{
    const char *key;
    String b(APP_FULLNAME);
    b.append(":",1);
    key = storeEntry()->getMD5Text();
    b.append(key, strlen(key));
    return b;
}

/** adds appropriate Range headers if needed */
void
ClientSocketContext::buildRangeHeader(HttpReply * rep)
{
    HttpHeader *hdr = rep ? &rep->header : 0;
    const char *range_err = NULL;
    HttpRequest *request = http->request;
    assert(request->range);
    /* check if we still want to do ranges */

    int64_t roffLimit = request->getRangeOffsetLimit();

    if (!rep)
        range_err = "no [parse-able] reply";
    else if ((rep->sline.status() != Http::scOkay) && (rep->sline.status() != Http::scPartialContent))
        range_err = "wrong status code";
    else if (hdr->has(HDR_CONTENT_RANGE))
        range_err = "origin server does ranges";
    else if (rep->content_length < 0)
        range_err = "unknown length";
    else if (rep->content_length != http->memObject()->getReply()->content_length)
        range_err = "INCONSISTENT length";  /* a bug? */

    /* hits only - upstream CachePeer determines correct behaviour on misses, and client_side_reply determines
     * hits candidates
     */
    else if (logTypeIsATcpHit(http->logType) && http->request->header.has(HDR_IF_RANGE) && !clientIfRangeMatch(http, rep))
        range_err = "If-Range match failed";
    else if (!http->request->range->canonize(rep))
        range_err = "canonization failed";
    else if (http->request->range->isComplex())
        range_err = "too complex range header";
    else if (!logTypeIsATcpHit(http->logType) && http->request->range->offsetLimitExceeded(roffLimit))
        range_err = "range outside range_offset_limit";

    /* get rid of our range specs on error */
    if (range_err) {
        /* XXX We do this here because we need canonisation etc. However, this current
         * code will lead to incorrect store offset requests - the store will have the
         * offset data, but we won't be requesting it.
         * So, we can either re-request, or generate an error
         */
        http->request->ignoreRange(range_err);
    } else {
        /* XXX: TODO: Review, this unconditional set may be wrong. */
        rep->sline.set(rep->sline.version, Http::scPartialContent);
        // web server responded with a valid, but unexpected range.
        // will (try-to) forward as-is.
        //TODO: we should cope with multirange request/responses
        bool replyMatchRequest = rep->content_range != NULL ?
                                 request->range->contains(rep->content_range->spec) :
                                 true;
        const int spec_count = http->request->range->specs.size();
        int64_t actual_clen = -1;

        debugs(33, 3, "clientBuildRangeHeader: range spec count: " <<
               spec_count << " virgin clen: " << rep->content_length);
        assert(spec_count > 0);
        /* append appropriate header(s) */

        if (spec_count == 1) {
            if (!replyMatchRequest) {
                hdr->delById(HDR_CONTENT_RANGE);
                hdr->putContRange(rep->content_range);
                actual_clen = rep->content_length;
                //http->range_iter.pos = rep->content_range->spec.begin();
                (*http->range_iter.pos)->offset = rep->content_range->spec.offset;
                (*http->range_iter.pos)->length = rep->content_range->spec.length;

            } else {
                HttpHdrRange::iterator pos = http->request->range->begin();
                assert(*pos);
                /* append Content-Range */

                if (!hdr->has(HDR_CONTENT_RANGE)) {
                    /* No content range, so this was a full object we are
                     * sending parts of.
                     */
                    httpHeaderAddContRange(hdr, **pos, rep->content_length);
                }

                /* set new Content-Length to the actual number of bytes
                 * transmitted in the message-body */
                actual_clen = (*pos)->length;
            }
        } else {
            /* multipart! */
            /* generate boundary string */
            http->range_iter.boundary = http->rangeBoundaryStr();
            /* delete old Content-Type, add ours */
            hdr->delById(HDR_CONTENT_TYPE);
            httpHeaderPutStrf(hdr, HDR_CONTENT_TYPE,
                              "multipart/byteranges; boundary=\"" SQUIDSTRINGPH "\"",
                              SQUIDSTRINGPRINT(http->range_iter.boundary));
            /* Content-Length is not required in multipart responses
             * but it is always nice to have one */
            actual_clen = http->mRangeCLen();
            /* http->out needs to start where we want data at */
            http->out.offset = http->range_iter.currentSpec()->offset;
        }

        /* replace Content-Length header */
        assert(actual_clen >= 0);

        hdr->delById(HDR_CONTENT_LENGTH);

        hdr->putInt64(HDR_CONTENT_LENGTH, actual_clen);

        debugs(33, 3, "clientBuildRangeHeader: actual content length: " << actual_clen);

        /* And start the range iter off */
        http->range_iter.updateSpec();
    }
}

void
ClientSocketContext::prepareReply(HttpReply * rep)
{
    reply = rep;

    if (http->request->range)
        buildRangeHeader(rep);
}

void
ClientSocketContext::sendStartOfMessage(HttpReply * rep, StoreIOBuffer bodyData)
{
    prepareReply(rep);
    assert (rep);
    MemBuf *mb = rep->pack();

    // dump now, so we dont output any body.
    debugs(11, 2, "HTTP Client " << clientConnection);
    debugs(11, 2, "HTTP Client REPLY:\n---------\n" << mb->buf << "\n----------");

    /* Save length of headers for persistent conn checks */
    http->out.headers_sz = mb->contentSize();
#if HEADERS_LOG

    headersLog(0, 0, http->request->method, rep);
#endif

    if (bodyData.data && bodyData.length) {
        if (multipartRangeRequest())
            packRange(bodyData, mb);
        else if (http->request->flags.chunkedReply) {
            packChunk(bodyData, *mb);
        } else {
            size_t length = lengthToSend(bodyData.range());
            noteSentBodyBytes (length);

            mb->append(bodyData.data, length);
        }
    }

    /* write */
    debugs(33,7, HERE << "sendStartOfMessage schedules clientWriteComplete");
    AsyncCall::Pointer call = commCbCall(33, 5, "clientWriteComplete",
                                         CommIoCbPtrFun(clientWriteComplete, this));
    Comm::Write(clientConnection, mb, call);
    delete mb;
}

/**
 * Write a chunk of data to a client socket. If the reply is present,
 * send the reply headers down the wire too, and clean them up when
 * finished.
 * Pre-condition:
 *   The request is one backed by a connection, not an internal request.
 *   data context is not NULL
 *   There are no more entries in the stream chain.
 */
void
clientSocketRecipient(clientStreamNode * node, ClientHttpRequest * http,
                      HttpReply * rep, StoreIOBuffer receivedData)
{
    /* Test preconditions */
    assert(node != NULL);
    PROF_start(clientSocketRecipient);
    /* TODO: handle this rather than asserting
     * - it should only ever happen if we cause an abort and
     * the callback chain loops back to here, so we can simply return.
     * However, that itself shouldn't happen, so it stays as an assert for now.
     */
    assert(cbdataReferenceValid(node));
    assert(node->node.next == NULL);
    ClientSocketContext::Pointer context = dynamic_cast<ClientSocketContext *>(node->data.getRaw());
    assert(context != NULL);
    assert(connIsUsable(http->getConn()));

    /* TODO: check offset is what we asked for */

    if (context != http->getConn()->getCurrentContext())
        context->deferRecipientForLater(node, rep, receivedData);
    else
        http->getConn()->handleReply(rep, receivedData);

    PROF_stop(clientSocketRecipient);
}

/**
 * Called when a downstream node is no longer interested in
 * our data. As we are a terminal node, this means on aborts
 * only
 */
void
clientSocketDetach(clientStreamNode * node, ClientHttpRequest * http)
{
    /* Test preconditions */
    assert(node != NULL);
    /* TODO: handle this rather than asserting
     * - it should only ever happen if we cause an abort and
     * the callback chain loops back to here, so we can simply return.
     * However, that itself shouldn't happen, so it stays as an assert for now.
     */
    assert(cbdataReferenceValid(node));
    /* Set null by ContextFree */
    assert(node->node.next == NULL);
    /* this is the assert discussed above */
    assert(NULL == dynamic_cast<ClientSocketContext *>(node->data.getRaw()));
    /* We are only called when the client socket shutsdown.
     * Tell the prev pipeline member we're finished
     */
    clientStreamDetach(node, http);
}

static void
clientWriteBodyComplete(const Comm::ConnectionPointer &conn, char *, size_t size, Comm::Flag errflag, int xerrno, void *data)
{
    debugs(33,7, "schedule clientWriteComplete");
    clientWriteComplete(conn, NULL, size, errflag, xerrno, data);
}

void
ConnStateData::readNextRequest()
{
    debugs(33, 5, HERE << clientConnection << " reading next req");

    fd_note(clientConnection->fd, "Idle client: Waiting for next request");
    /**
     * Set the timeout BEFORE calling readSomeData().
     */
    typedef CommCbMemFunT<ConnStateData, CommTimeoutCbParams> TimeoutDialer;
    AsyncCall::Pointer timeoutCall = JobCallback(33, 5,
                                     TimeoutDialer, this, ConnStateData::requestTimeout);
    commSetConnTimeout(clientConnection, clientConnection->timeLeft(idleTimeout()), timeoutCall);

    readSomeData();
    /** Please don't do anything with the FD past here! */
}

static void
ClientSocketContextPushDeferredIfNeeded(ClientSocketContext::Pointer deferredRequest, ConnStateData * conn)
{
    debugs(33, 2, HERE << conn->clientConnection << " Sending next");

    /** If the client stream is waiting on a socket write to occur, then */

    if (deferredRequest->flags.deferred) {
        /** NO data is allowed to have been sent. */
        assert(deferredRequest->http->out.size == 0);
        /** defer now. */
        clientSocketRecipient(deferredRequest->deferredparams.node,
                              deferredRequest->http,
                              deferredRequest->deferredparams.rep,
                              deferredRequest->deferredparams.queuedBuffer);
    }

    /** otherwise, the request is still active in a callbacksomewhere,
     * and we are done
     */
}

/// called when we have successfully finished writing the response
void
ClientSocketContext::keepaliveNextRequest()
{
    ConnStateData * conn = http->getConn();

    debugs(33, 3, HERE << "ConnnStateData(" << conn->clientConnection << "), Context(" << clientConnection << ")");
    connIsFinished();

    if (conn->pinning.pinned && !Comm::IsConnOpen(conn->pinning.serverConnection)) {
        debugs(33, 2, HERE << conn->clientConnection << " Connection was pinned but server side gone. Terminating client connection");
        conn->clientConnection->close();
        return;
    }

    /** \par
     * We are done with the response, and we are either still receiving request
     * body (early response!) or have already stopped receiving anything.
     *
     * If we are still receiving, then clientParseRequest() below will fail.
     * (XXX: but then we will call readNextRequest() which may succeed and
     * execute a smuggled request as we are not done with the current request).
     *
     * If we stopped because we got everything, then try the next request.
     *
     * If we stopped receiving because of an error, then close now to avoid
     * getting stuck and to prevent accidental request smuggling.
     */

    if (const char *reason = conn->stoppedReceiving()) {
        debugs(33, 3, HERE << "closing for earlier request error: " << reason);
        conn->clientConnection->close();
        return;
    }

    /** \par
     * Attempt to parse a request from the request buffer.
     * If we've been fed a pipelined request it may already
     * be in our read buffer.
     *
     \par
     * This needs to fall through - if we're unlucky and parse the _last_ request
     * from our read buffer we may never re-register for another client read.
     */

    if (conn->clientParseRequests()) {
        debugs(33, 3, HERE << conn->clientConnection << ": parsed next request from buffer");
    }

    /** \par
     * Either we need to kick-start another read or, if we have
     * a half-closed connection, kill it after the last request.
     * This saves waiting for half-closed connections to finished being
     * half-closed _AND_ then, sometimes, spending "Timeout" time in
     * the keepalive "Waiting for next request" state.
     */
    if (commIsHalfClosed(conn->clientConnection->fd) && (conn->getConcurrentRequestCount() == 0)) {
        debugs(33, 3, "ClientSocketContext::keepaliveNextRequest: half-closed client with no pending requests, closing");
        conn->clientConnection->close();
        return;
    }

    ClientSocketContext::Pointer deferredRequest;

    /** \par
     * At this point we either have a parsed request (which we've
     * kicked off the processing for) or not. If we have a deferred
     * request (parsed but deferred for pipeling processing reasons)
     * then look at processing it. If not, simply kickstart
     * another read.
     */

    if ((deferredRequest = conn->getCurrentContext()).getRaw()) {
        debugs(33, 3, HERE << conn->clientConnection << ": calling PushDeferredIfNeeded");
        ClientSocketContextPushDeferredIfNeeded(deferredRequest, conn);
    } else if (conn->flags.readMore) {
        debugs(33, 3, HERE << conn->clientConnection << ": calling conn->readNextRequest()");
        conn->readNextRequest();
    } else {
        // XXX: Can this happen? CONNECT tunnels have deferredRequest set.
        debugs(33, DBG_IMPORTANT, HERE << "abandoning " << conn->clientConnection);
    }
}

void
clientUpdateSocketStats(LogTags logType, size_t size)
{
    if (size == 0)
        return;

    kb_incr(&statCounter.client_http.kbytes_out, size);

    if (logTypeIsATcpHit(logType))
        kb_incr(&statCounter.client_http.hit_kbytes_out, size);
}

/**
 * increments iterator "i"
 * used by clientPackMoreRanges
 *
 \retval true    there is still data available to pack more ranges
 \retval false
 */
bool
ClientSocketContext::canPackMoreRanges() const
{
    /** first update iterator "i" if needed */

    if (!http->range_iter.debt()) {
        debugs(33, 5, HERE << "At end of current range spec for " << clientConnection);

        if (http->range_iter.pos != http->range_iter.end)
            ++http->range_iter.pos;

        http->range_iter.updateSpec();
    }

    assert(!http->range_iter.debt() == !http->range_iter.currentSpec());

    /* paranoid sync condition */
    /* continue condition: need_more_data */
    debugs(33, 5, "ClientSocketContext::canPackMoreRanges: returning " << (http->range_iter.currentSpec() ? true : false));
    return http->range_iter.currentSpec() ? true : false;
}

int64_t
ClientSocketContext::getNextRangeOffset() const
{
    debugs (33, 5, "range: " << http->request->range <<
            "; http offset " << http->out.offset <<
            "; reply " << reply);

    // XXX: This method is called from many places, including pullData() which
    // may be called before prepareReply() [on some Squid-generated errors].
    // Hence, we may not even know yet whether we should honor/do ranges.

    if (http->request->range) {
        /* offset in range specs does not count the prefix of an http msg */
        /* check: reply was parsed and range iterator was initialized */
        assert(http->range_iter.valid);
        /* filter out data according to range specs */
        assert (canPackMoreRanges());
        {
            int64_t start;      /* offset of still missing data */
            assert(http->range_iter.currentSpec());
            start = http->range_iter.currentSpec()->offset + http->range_iter.currentSpec()->length - http->range_iter.debt();
            debugs(33, 3, "clientPackMoreRanges: in:  offset: " << http->out.offset);
            debugs(33, 3, "clientPackMoreRanges: out:"
                   " start: " << start <<
                   " spec[" << http->range_iter.pos - http->request->range->begin() << "]:" <<
                   " [" << http->range_iter.currentSpec()->offset <<
                   ", " << http->range_iter.currentSpec()->offset + http->range_iter.currentSpec()->length << "),"
                   " len: " << http->range_iter.currentSpec()->length <<
                   " debt: " << http->range_iter.debt());
            if (http->range_iter.currentSpec()->length != -1)
                assert(http->out.offset <= start);  /* we did not miss it */

            return start;
        }

    } else if (reply && reply->content_range) {
        /* request does not have ranges, but reply does */
        /** \todo FIXME: should use range_iter_pos on reply, as soon as reply->content_range
         *        becomes HttpHdrRange rather than HttpHdrRangeSpec.
         */
        return http->out.offset + reply->content_range->spec.offset;
    }

    return http->out.offset;
}

void
ClientSocketContext::pullData()
{
    debugs(33, 5, reply << " written " << http->out.size << " into " << clientConnection);

    /* More data will be coming from the stream. */
    StoreIOBuffer readBuffer;
    /* XXX: Next requested byte in the range sequence */
    /* XXX: length = getmaximumrangelenfgth */
    readBuffer.offset = getNextRangeOffset();
    readBuffer.length = HTTP_REQBUF_SZ;
    readBuffer.data = reqbuf;
    /* we may note we have reached the end of the wanted ranges */
    clientStreamRead(getTail(), http, readBuffer);
}

/** Adapt stream status to account for Range cases
 *
 */
clientStream_status_t
ClientSocketContext::socketState()
{
    switch (clientStreamStatus(getTail(), http)) {

    case STREAM_NONE:
        /* check for range support ending */

        if (http->request->range) {
            /* check: reply was parsed and range iterator was initialized */
            assert(http->range_iter.valid);
            /* filter out data according to range specs */

            if (!canPackMoreRanges()) {
                debugs(33, 5, HERE << "Range request at end of returnable " <<
                       "range sequence on " << clientConnection);
                // we got everything we wanted from the store
                return STREAM_COMPLETE;
            }
        } else if (reply && reply->content_range) {
            /* reply has content-range, but Squid is not managing ranges */
            const int64_t &bytesSent = http->out.offset;
            const int64_t &bytesExpected = reply->content_range->spec.length;

            debugs(33, 7, HERE << "body bytes sent vs. expected: " <<
                   bytesSent << " ? " << bytesExpected << " (+" <<
                   reply->content_range->spec.offset << ")");

            // did we get at least what we expected, based on range specs?

            if (bytesSent == bytesExpected) // got everything
                return STREAM_COMPLETE;

            if (bytesSent > bytesExpected) // Error: Sent more than expected
                return STREAM_UNPLANNED_COMPLETE;
        }

        return STREAM_NONE;

    case STREAM_COMPLETE:
        return STREAM_COMPLETE;

    case STREAM_UNPLANNED_COMPLETE:
        return STREAM_UNPLANNED_COMPLETE;

    case STREAM_FAILED:
        return STREAM_FAILED;
    }

    fatal ("unreachable code\n");
    return STREAM_NONE;
}

/**
 * A write has just completed to the client, or we have just realised there is
 * no more data to send.
 */
void
clientWriteComplete(const Comm::ConnectionPointer &conn, char *bufnotused, size_t size, Comm::Flag errflag, int, void *data)
{
    ClientSocketContext *context = (ClientSocketContext *)data;
    context->writeComplete(conn, bufnotused, size, errflag);
}

/// remembers the abnormal connection termination for logging purposes
void
ClientSocketContext::noteIoError(const int xerrno)
{
    if (http) {
        if (xerrno == ETIMEDOUT)
            http->al->http.timedout = true;
        else // even if xerrno is zero (which means read abort/eof)
            http->al->http.aborted = true;
    }
}

void
ClientSocketContext::doClose()
{
    clientConnection->close();
}

/// called when we encounter a response-related error
void
ClientSocketContext::initiateClose(const char *reason)
{
    http->getConn()->stopSending(reason); // closes ASAP
}

void
ConnStateData::stopSending(const char *error)
{
    debugs(33, 4, HERE << "sending error (" << clientConnection << "): " << error <<
           "; old receiving error: " <<
           (stoppedReceiving() ? stoppedReceiving_ : "none"));

    if (const char *oldError = stoppedSending()) {
        debugs(33, 3, HERE << "already stopped sending: " << oldError);
        return; // nothing has changed as far as this connection is concerned
    }
    stoppedSending_ = error;

    if (!stoppedReceiving()) {
        if (const int64_t expecting = mayNeedToReadMoreBody()) {
            debugs(33, 5, HERE << "must still read " << expecting <<
                   " request body bytes with " << in.buf.length() << " unused");
            return; // wait for the request receiver to finish reading
        }
    }

    clientConnection->close();
}

void
ClientSocketContext::writeComplete(const Comm::ConnectionPointer &conn, char *, size_t size, Comm::Flag errflag)
{
    const StoreEntry *entry = http->storeEntry();
    http->out.size += size;
    debugs(33, 5, HERE << conn << ", sz " << size <<
           ", err " << errflag << ", off " << http->out.size << ", len " <<
           (entry ? entry->objectLen() : 0));
    clientUpdateSocketStats(http->logType, size);

    /* Bail out quickly on Comm::ERR_CLOSING - close handlers will tidy up */

    if (errflag == Comm::ERR_CLOSING || !Comm::IsConnOpen(conn))
        return;

    if (errflag || clientHttpRequestStatus(conn->fd, http)) {
        initiateClose("failure or true request status");
        /* Do we leak here ? */
        return;
    }

    switch (socketState()) {

    case STREAM_NONE:
        pullData();
        break;

    case STREAM_COMPLETE:
        debugs(33, 5, conn << "Stream complete, keepalive is " << http->request->flags.proxyKeepalive);
        if (http->request->flags.proxyKeepalive)
            keepaliveNextRequest();
        else
            initiateClose("STREAM_COMPLETE NOKEEPALIVE");
        return;

    case STREAM_UNPLANNED_COMPLETE:
        initiateClose("STREAM_UNPLANNED_COMPLETE");
        return;

    case STREAM_FAILED:
        initiateClose("STREAM_FAILED");
        return;

    default:
        fatal("Hit unreachable code in clientWriteComplete\n");
    }
}

ClientSocketContext *
ConnStateData::abortRequestParsing(const char *const uri)
{
    ClientHttpRequest *http = new ClientHttpRequest(this);
    http->req_sz = in.buf.length();
    http->uri = xstrdup(uri);
    setLogUri (http, uri);
    ClientSocketContext *context = new ClientSocketContext(clientConnection, http);
    StoreIOBuffer tempBuffer;
    tempBuffer.data = context->reqbuf;
    tempBuffer.length = HTTP_REQBUF_SZ;
    clientStreamInit(&http->client_stream, clientGetMoreData, clientReplyDetach,
                     clientReplyStatus, new clientReplyContext(http), clientSocketRecipient,
                     clientSocketDetach, context, tempBuffer);
    return context;
}

char *
skipLeadingSpace(char *aString)
{
    char *result = aString;

    while (xisspace(*aString))
        ++aString;

    return result;
}

/**
 * 'end' defaults to NULL for backwards compatibility
 * remove default value if we ever get rid of NULL-terminated
 * request buffers.
 */
const char *
findTrailingHTTPVersion(const char *uriAndHTTPVersion, const char *end)
{
    if (NULL == end) {
        end = uriAndHTTPVersion + strcspn(uriAndHTTPVersion, "\r\n");
        assert(end);
    }

    for (; end > uriAndHTTPVersion; --end) {
        if (*end == '\n' || *end == '\r')
            continue;

        if (xisspace(*end)) {
            if (strncasecmp(end + 1, "HTTP/", 5) == 0)
                return end + 1;
            else
                break;
        }
    }

    return NULL;
}

void
setLogUri(ClientHttpRequest * http, char const *uri, bool cleanUrl)
{
    safe_free(http->log_uri);

    if (!cleanUrl)
        // The uri is already clean just dump it.
        http->log_uri = xstrndup(uri, MAX_URL);
    else {
        int flags = 0;
        switch (Config.uri_whitespace) {
        case URI_WHITESPACE_ALLOW:
            flags |= RFC1738_ESCAPE_NOSPACE;

        case URI_WHITESPACE_ENCODE:
            flags |= RFC1738_ESCAPE_UNESCAPED;
            http->log_uri = xstrndup(rfc1738_do_escape(uri, flags), MAX_URL);
            break;

        case URI_WHITESPACE_CHOP: {
            flags |= RFC1738_ESCAPE_NOSPACE;
            flags |= RFC1738_ESCAPE_UNESCAPED;
            http->log_uri = xstrndup(rfc1738_do_escape(uri, flags), MAX_URL);
            int pos = strcspn(http->log_uri, w_space);
            http->log_uri[pos] = '\0';
        }
        break;

        case URI_WHITESPACE_DENY:
        case URI_WHITESPACE_STRIP:
        default: {
            const char *t;
            char *tmp_uri = static_cast<char*>(xmalloc(strlen(uri) + 1));
            char *q = tmp_uri;
            t = uri;
            while (*t) {
                if (!xisspace(*t)) {
                    *q = *t;
                    ++q;
                }
                ++t;
            }
            *q = '\0';
            http->log_uri = xstrndup(rfc1738_escape_unescaped(tmp_uri), MAX_URL);
            xfree(tmp_uri);
        }
        break;
        }
    }
}

static void
prepareAcceleratedURL(ConnStateData * conn, ClientHttpRequest *http, const Http1::RequestParserPointer &hp)
{
    int vhost = conn->port->vhost;
    int vport = conn->port->vport;
    static char ipbuf[MAX_IPSTRLEN];

    http->flags.accel = true;

    /* BUG: Squid cannot deal with '*' URLs (RFC2616 5.1.2) */

    static const SBuf cache_object("cache_object://");
    if (hp->requestUri().startsWith(cache_object))
        return; /* already in good shape */

    // XXX: re-use proper URL parser for this
    SBuf url = hp->requestUri(); // use full provided URI if we abort
    do { // use a loop so we can break out of it
        ::Parser::Tokenizer tok(url);
        if (tok.skip('/')) // origin-form URL already.
            break;

        if (conn->port->vhost)
            return; /* already in good shape */

        // skip the URI scheme
        static const CharacterSet uriScheme = CharacterSet("URI-scheme","+-.") + CharacterSet::ALPHA + CharacterSet::DIGIT;
        static const SBuf uriSchemeEnd("://");
        if (!tok.skipAll(uriScheme) || !tok.skip(uriSchemeEnd))
            break;

        // skip the authority segment
        // RFC 3986 complex nested ABNF for "authority" boils down to this:
        static const CharacterSet authority = CharacterSet("authority","-._~%:@[]!$&'()*+,;=") +
                                              CharacterSet::HEXDIG + CharacterSet::ALPHA + CharacterSet::DIGIT;
        if (!tok.skipAll(authority))
            break;

        static const SBuf slashUri("/");
        const SBuf t = tok.remaining();
        if (t.isEmpty())
            url = slashUri;
        else if (t[0]=='/') // looks like path
            url = t;
        else if (t[0]=='?' || t[0]=='#') { // looks like query or fragment. fix '/'
            url = slashUri;
            url.append(t);
        } // else do nothing. invalid path

    } while(false);

#if SHOULD_REJECT_UNKNOWN_URLS
    // reject URI which are not well-formed even after the processing above
    if (url.isEmpty() || url[0] != '/') {
        hp->parseStatusCode = Http::scBadRequest;
        return conn->abortRequestParsing("error:invalid-request");
    }
#endif

    if (vport < 0)
        vport = http->getConn()->clientConnection->local.port();

    const bool switchedToHttps = conn->switchedToHttps();
    const bool tryHostHeader = vhost || switchedToHttps;
    char *host = NULL;
    if (tryHostHeader && (host = hp->getHeaderField("Host"))) {
        debugs(33, 5, "ACCEL VHOST REWRITE: vhost=" << host << " + vport=" << vport);
        char thost[256];
        if (vport > 0) {
            thost[0] = '\0';
            char *t = NULL;
            if (host[strlen(host)] != ']' && (t = strrchr(host,':')) != NULL) {
                strncpy(thost, host, (t-host));
                snprintf(thost+(t-host), sizeof(thost)-(t-host), ":%d", vport);
                host = thost;
            } else if (!t) {
                snprintf(thost, sizeof(thost), "%s:%d",host, vport);
                host = thost;
            }
        } // else nothing to alter port-wise.
        const int url_sz = hp->requestUri().length() + 32 + Config.appendDomainLen + strlen(host);
        http->uri = (char *)xcalloc(url_sz, 1);
        snprintf(http->uri, url_sz, "%s://%s" SQUIDSBUFPH, AnyP::UriScheme(conn->transferProtocol.protocol).c_str(), host, SQUIDSBUFPRINT(url));
        debugs(33, 5, "ACCEL VHOST REWRITE: " << http->uri);
    } else if (conn->port->defaultsite /* && !vhost */) {
        debugs(33, 5, "ACCEL DEFAULTSITE REWRITE: defaultsite=" << conn->port->defaultsite << " + vport=" << vport);
        const int url_sz = hp->requestUri().length() + 32 + Config.appendDomainLen +
                           strlen(conn->port->defaultsite);
        http->uri = (char *)xcalloc(url_sz, 1);
        char vportStr[32];
        vportStr[0] = '\0';
        if (vport > 0) {
            snprintf(vportStr, sizeof(vportStr),":%d",vport);
        }
        snprintf(http->uri, url_sz, "%s://%s%s" SQUIDSBUFPH,
                 AnyP::UriScheme(conn->transferProtocol.protocol).c_str(), conn->port->defaultsite, vportStr, SQUIDSBUFPRINT(url));
        debugs(33, 5, "ACCEL DEFAULTSITE REWRITE: " << http->uri);
    } else if (vport > 0 /* && (!vhost || no Host:) */) {
        debugs(33, 5, "ACCEL VPORT REWRITE: *_port IP + vport=" << vport);
        /* Put the local socket IP address as the hostname, with whatever vport we found  */
        const int url_sz = hp->requestUri().length() + 32 + Config.appendDomainLen;
        http->uri = (char *)xcalloc(url_sz, 1);
        http->getConn()->clientConnection->local.toHostStr(ipbuf,MAX_IPSTRLEN);
        snprintf(http->uri, url_sz, "%s://%s:%d" SQUIDSBUFPH,
                 AnyP::UriScheme(conn->transferProtocol.protocol).c_str(),
                 ipbuf, vport, SQUIDSBUFPRINT(url));
        debugs(33, 5, "ACCEL VPORT REWRITE: " << http->uri);
    }
}

static void
prepareTransparentURL(ConnStateData * conn, ClientHttpRequest *http, const Http1::RequestParserPointer &hp)
{
    // TODO Must() on URI !empty when the parser supports throw. For now avoid assert().
    if (!hp->requestUri().isEmpty() && hp->requestUri()[0] != '/')
        return; /* already in good shape */

    /* BUG: Squid cannot deal with '*' URLs (RFC2616 5.1.2) */

    if (const char *host = hp->getHeaderField("Host")) {
        const int url_sz = hp->requestUri().length() + 32 + Config.appendDomainLen +
                           strlen(host);
        http->uri = (char *)xcalloc(url_sz, 1);
        snprintf(http->uri, url_sz, "%s://%s" SQUIDSBUFPH,
                 AnyP::UriScheme(conn->transferProtocol.protocol).c_str(), host, SQUIDSBUFPRINT(hp->requestUri()));
        debugs(33, 5, "TRANSPARENT HOST REWRITE: " << http->uri);
    } else {
        /* Put the local socket IP address as the hostname.  */
        const int url_sz = hp->requestUri().length() + 32 + Config.appendDomainLen;
        http->uri = (char *)xcalloc(url_sz, 1);
        static char ipbuf[MAX_IPSTRLEN];
        http->getConn()->clientConnection->local.toHostStr(ipbuf,MAX_IPSTRLEN);
        snprintf(http->uri, url_sz, "%s://%s:%d" SQUIDSBUFPH,
                 AnyP::UriScheme(http->getConn()->transferProtocol.protocol).c_str(),
                 ipbuf, http->getConn()->clientConnection->local.port(), SQUIDSBUFPRINT(hp->requestUri()));
        debugs(33, 5, "TRANSPARENT REWRITE: " << http->uri);
    }
}

/** Parse an HTTP request
 *
 *  \note Sets result->flags.parsed_ok to 0 if failed to parse the request,
 *          to 1 if the request was correctly parsed.
 *  \param[in] csd a ConnStateData. The caller must make sure it is not null
 *  \param[in] hp an Http1::RequestParser
 *  \param[out] mehtod_p will be set as a side-effect of the parsing.
 *          Pointed-to value will be set to Http::METHOD_NONE in case of
 *          parsing failure
 *  \param[out] http_ver will be set as a side-effect of the parsing
 *  \return NULL on incomplete requests,
 *          a ClientSocketContext structure on success or failure.
 */
ClientSocketContext *
parseHttpRequest(ConnStateData *csd, const Http1::RequestParserPointer &hp)
{
    /* Attempt to parse the first line; this will define where the method, url, version and header begin */
    {
        const bool parsedOk = hp->parse(csd->in.buf);

        if (csd->port->flags.isIntercepted() && Config.accessList.on_unsupported_protocol)
            csd->preservedClientData = csd->in.buf;
        // sync the buffers after parsing.
        csd->in.buf = hp->remaining();

        if (hp->needsMoreData()) {
            debugs(33, 5, "Incomplete request, waiting for end of request line");
            return NULL;
        }

        if (!parsedOk) {
            if (hp->parseStatusCode == Http::scRequestHeaderFieldsTooLarge || hp->parseStatusCode == Http::scUriTooLong)
                return csd->abortRequestParsing("error:request-too-large");

            return csd->abortRequestParsing("error:invalid-request");
        }
    }

    /* We know the whole request is in parser now */
    debugs(11, 2, "HTTP Client " << csd->clientConnection);
    debugs(11, 2, "HTTP Client REQUEST:\n---------\n" <<
           hp->method() << " " << hp->requestUri() << " " << hp->messageProtocol() << "\n" <<
           hp->mimeHeader() <<
           "\n----------");

    /* deny CONNECT via accelerated ports */
    if (hp->method() == Http::METHOD_CONNECT && csd->port != NULL && csd->port->flags.accelSurrogate) {
        debugs(33, DBG_IMPORTANT, "WARNING: CONNECT method received on " << csd->transferProtocol << " Accelerator port " << csd->port->s.port());
        debugs(33, DBG_IMPORTANT, "WARNING: for request: " << hp->method() << " " << hp->requestUri() << " " << hp->messageProtocol());
        hp->parseStatusCode = Http::scMethodNotAllowed;
        return csd->abortRequestParsing("error:method-not-allowed");
    }

    /* draft-ietf-httpbis-http2-16 section 11.6 registers the method PRI as HTTP/2 specific
     * Deny "PRI" method if used in HTTP/1.x or 0.9 versions.
     * If seen it signals a broken client or proxy has corrupted the traffic.
     */
    if (hp->method() == Http::METHOD_PRI && hp->messageProtocol() < Http::ProtocolVersion(2,0)) {
        debugs(33, DBG_IMPORTANT, "WARNING: PRI method received on " << csd->transferProtocol << " port " << csd->port->s.port());
        debugs(33, DBG_IMPORTANT, "WARNING: for request: " << hp->method() << " " << hp->requestUri() << " " << hp->messageProtocol());
        hp->parseStatusCode = Http::scMethodNotAllowed;
        return csd->abortRequestParsing("error:method-not-allowed");
    }

    if (hp->method() == Http::METHOD_NONE) {
        debugs(33, DBG_IMPORTANT, "WARNING: Unsupported method: " << hp->method() << " " << hp->requestUri() << " " << hp->messageProtocol());
        hp->parseStatusCode = Http::scMethodNotAllowed;
        return csd->abortRequestParsing("error:unsupported-request-method");
    }

    // Process headers after request line
    debugs(33, 3, "complete request received. " <<
           "prefix_sz = " << hp->messageHeaderSize() <<
           ", request-line-size=" << hp->firstLineSize() <<
           ", mime-header-size=" << hp->headerBlockSize() <<
           ", mime header block:\n" << hp->mimeHeader() << "\n----------");

    /* Ok, all headers are received */
    ClientHttpRequest *http = new ClientHttpRequest(csd);

    http->req_sz = hp->messageHeaderSize();
    ClientSocketContext *result = new ClientSocketContext(csd->clientConnection, http);

    StoreIOBuffer tempBuffer;
    tempBuffer.data = result->reqbuf;
    tempBuffer.length = HTTP_REQBUF_SZ;

    ClientStreamData newServer = new clientReplyContext(http);
    ClientStreamData newClient = result;
    clientStreamInit(&http->client_stream, clientGetMoreData, clientReplyDetach,
                     clientReplyStatus, newServer, clientSocketRecipient,
                     clientSocketDetach, newClient, tempBuffer);

    /* set url */
    // XXX: c_str() does re-allocate but here replaces explicit malloc/free.
    // when internalCheck() accepts SBuf removing this will be a net gain for performance.
    SBuf tmp(hp->requestUri());
    const char *url = tmp.c_str();

    debugs(33,5, HERE << "repare absolute URL from " <<
           (csd->transparent()?"intercept":(csd->port->flags.accelSurrogate ? "accel":"")));
    /* Rewrite the URL in transparent or accelerator mode */
    /* NP: there are several cases to traverse here:
     *  - standard mode (forward proxy)
     *  - transparent mode (TPROXY)
     *  - transparent mode with failures
     *  - intercept mode (NAT)
     *  - intercept mode with failures
     *  - accelerator mode (reverse proxy)
     *  - internal URL
     *  - mixed combos of the above with internal URL
     *  - remote interception with PROXY protocol
     *  - remote reverse-proxy with PROXY protocol
     */
    if (csd->transparent()) {
        /* intercept or transparent mode, properly working with no failures */
        prepareTransparentURL(csd, http, hp);

    } else if (internalCheck(url)) {
        /* internal URL mode */
        /* prepend our name & port */
        http->uri = xstrdup(internalLocalUri(NULL, url));
        // We just re-wrote the URL. Must replace the Host: header.
        //  But have not parsed there yet!! flag for local-only handling.
        http->flags.internal = true;

    } else if (csd->port->flags.accelSurrogate || csd->switchedToHttps()) {
        /* accelerator mode */
        prepareAcceleratedURL(csd, http, hp);
    }

    if (!http->uri) {
        /* No special rewrites have been applied above, use the
         * requested url. may be rewritten later, so make extra room */
        int url_sz = hp->requestUri().length() + Config.appendDomainLen + 5;
        http->uri = (char *)xcalloc(url_sz, 1);
        strcpy(http->uri, url);
    }

    result->flags.parsed_ok = 1;
    return result;
}

bool
ConnStateData::In::maybeMakeSpaceAvailable()
{
    if (buf.spaceSize() < 2) {
        const SBuf::size_type haveCapacity = buf.length() + buf.spaceSize();
        if (haveCapacity >= Config.maxRequestBufferSize) {
            debugs(33, 4, "request buffer full: client_request_buffer_max_size=" << Config.maxRequestBufferSize);
            return false;
        }
        if (haveCapacity == 0) {
            // haveCapacity is based on the SBuf visible window of the MemBlob buffer, which may fill up.
            // at which point bump the buffer back to default. This allocates a new MemBlob with any un-parsed bytes.
            buf.reserveCapacity(CLIENT_REQ_BUF_SZ);
        } else {
            const SBuf::size_type wantCapacity = min(static_cast<SBuf::size_type>(Config.maxRequestBufferSize), haveCapacity*2);
            buf.reserveCapacity(wantCapacity);
        }
        debugs(33, 2, "growing request buffer: available=" << buf.spaceSize() << " used=" << buf.length());
    }
    return (buf.spaceSize() >= 2);
}

void
ConnStateData::addContextToQueue(ClientSocketContext * context)
{
    ClientSocketContext::Pointer *S;

    for (S = (ClientSocketContext::Pointer *) & currentobject; S->getRaw();
            S = &(*S)->next);
    *S = context;

    ++nrequests;
}

int
ConnStateData::getConcurrentRequestCount() const
{
    int result = 0;
    ClientSocketContext::Pointer *T;

    for (T = (ClientSocketContext::Pointer *) &currentobject;
            T->getRaw(); T = &(*T)->next, ++result);
    return result;
}

int
ConnStateData::connFinishedWithConn(int size)
{
    if (size == 0) {
        if (getConcurrentRequestCount() == 0 && in.buf.isEmpty()) {
            /* no current or pending requests */
            debugs(33, 4, HERE << clientConnection << " closed");
            return 1;
        } else if (!Config.onoff.half_closed_clients) {
            /* admin doesn't want to support half-closed client sockets */
            debugs(33, 3, HERE << clientConnection << " aborted (half_closed_clients disabled)");
            notifyAllContexts(0); // no specific error implies abort
            return 1;
        }
    }

    return 0;
}

void
ConnStateData::consumeInput(const size_t byteCount)
{
    assert(byteCount > 0 && byteCount <= in.buf.length());
    in.buf.consume(byteCount);
    debugs(33, 5, "in.buf has " << in.buf.length() << " unused bytes");
}

void
ConnStateData::clientAfterReadingRequests()
{
    // Were we expecting to read more request body from half-closed connection?
    if (mayNeedToReadMoreBody() && commIsHalfClosed(clientConnection->fd)) {
        debugs(33, 3, HERE << "truncated body: closing half-closed " << clientConnection);
        clientConnection->close();
        return;
    }

    if (flags.readMore)
        readSomeData();
}

void
ConnStateData::quitAfterError(HttpRequest *request)
{
    // From HTTP p.o.v., we do not have to close after every error detected
    // at the client-side, but many such errors do require closure and the
    // client-side code is bad at handling errors so we play it safe.
    if (request)
        request->flags.proxyKeepalive = false;
    flags.readMore = false;
    debugs(33,4, HERE << "Will close after error: " << clientConnection);
}

#if USE_OPENSSL
bool ConnStateData::serveDelayedError(ClientSocketContext *context)
{
    ClientHttpRequest *http = context->http;

    if (!sslServerBump)
        return false;

    assert(sslServerBump->entry);
    // Did we create an error entry while processing CONNECT?
    if (!sslServerBump->entry->isEmpty()) {
        quitAfterError(http->request);

        // Get the saved error entry and send it to the client by replacing the
        // ClientHttpRequest store entry with it.
        clientStreamNode *node = context->getClientReplyContext();
        clientReplyContext *repContext = dynamic_cast<clientReplyContext *>(node->data.getRaw());
        assert(repContext);
        debugs(33, 5, "Responding with delated error for " << http->uri);
        repContext->setReplyToStoreEntry(sslServerBump->entry, "delayed SslBump error");

        // save the original request for logging purposes
        if (!context->http->al->request) {
            context->http->al->request = http->request;
            HTTPMSGLOCK(context->http->al->request);
        }

        // Get error details from the fake certificate-peeking request.
        http->request->detailError(sslServerBump->request->errType, sslServerBump->request->errDetail);
        context->pullData();
        return true;
    }

    // In bump-server-first mode, we have not necessarily seen the intended
    // server name at certificate-peeking time. Check for domain mismatch now,
    // when we can extract the intended name from the bumped HTTP request.
    if (X509 *srvCert = sslServerBump->serverCert.get()) {
        HttpRequest *request = http->request;
        if (!Ssl::checkX509ServerValidity(srvCert, request->GetHost())) {
            debugs(33, 2, "SQUID_X509_V_ERR_DOMAIN_MISMATCH: Certificate " <<
                   "does not match domainname " << request->GetHost());

            bool allowDomainMismatch = false;
            if (Config.ssl_client.cert_error) {
                ACLFilledChecklist check(Config.ssl_client.cert_error, request, dash_str);
                check.sslErrors = new Ssl::CertErrors(Ssl::CertError(SQUID_X509_V_ERR_DOMAIN_MISMATCH, srvCert));
                allowDomainMismatch = (check.fastCheck() == ACCESS_ALLOWED);
                delete check.sslErrors;
                check.sslErrors = NULL;
            }

            if (!allowDomainMismatch) {
                quitAfterError(request);

                clientStreamNode *node = context->getClientReplyContext();
                clientReplyContext *repContext = dynamic_cast<clientReplyContext *>(node->data.getRaw());
                assert (repContext);

                // Fill the server IP and hostname for error page generation.
                HttpRequest::Pointer const & peekerRequest = sslServerBump->request;
                request->hier.note(peekerRequest->hier.tcpServer, request->GetHost());

                // Create an error object and fill it
                ErrorState *err = new ErrorState(ERR_SECURE_CONNECT_FAIL, Http::scServiceUnavailable, request);
                err->src_addr = clientConnection->remote;
                Ssl::ErrorDetail *errDetail = new Ssl::ErrorDetail(
                    SQUID_X509_V_ERR_DOMAIN_MISMATCH,
                    srvCert, NULL);
                err->detail = errDetail;
                // Save the original request for logging purposes.
                if (!context->http->al->request) {
                    context->http->al->request = request;
                    HTTPMSGLOCK(context->http->al->request);
                }
                repContext->setReplyToError(request->method, err);
                assert(context->http->out.offset == 0);
                context->pullData();
                return true;
            }
        }
    }

    return false;
}
#endif // USE_OPENSSL

/**
 * Check on_unsupported_protocol checklist and return true if tunnel mode selected
 * or false otherwise
 */
bool
clientTunnelOnError(ConnStateData *conn, ClientSocketContext *context, HttpRequest *request, const HttpRequestMethod& method, err_type requestError, Http::StatusCode errStatusCode, const char *requestErrorBytes)
{
    if (conn->port->flags.isIntercepted() &&
            Config.accessList.on_unsupported_protocol && conn->nrequests <= 1) {
        ACLFilledChecklist checklist(Config.accessList.on_unsupported_protocol, request, NULL);
        checklist.requestErrorType = requestError;
        checklist.src_addr = conn->clientConnection->remote;
        checklist.my_addr = conn->clientConnection->local;
        checklist.conn(conn);
        allow_t answer = checklist.fastCheck();
        if (answer == ACCESS_ALLOWED && answer.kind == 1) {
            debugs(33, 3, "Request will be tunneled to server");
            if (context)
                context->removeFromConnectionList(conn);
            Comm::SetSelect(conn->clientConnection->fd, COMM_SELECT_READ, NULL, NULL, 0);

            SBuf preReadData;
            if (conn->preservedClientData.length())
                preReadData.append(conn->preservedClientData);
            static char ip[MAX_IPSTRLEN];
            conn->clientConnection->local.toUrl(ip, sizeof(ip));
            conn->in.buf.assign("CONNECT ").append(ip).append(" HTTP/1.1\r\nHost: ").append(ip).append("\r\n\r\n").append(preReadData);

            bool ret = conn->handleReadData();
            if (ret)
                ret = conn->clientParseRequests();

            if (!ret) {
                debugs(33, 2, "Failed to start fake CONNECT request for on_unsupported_protocol: " << conn->clientConnection);
                conn->clientConnection->close();
            }
            return true;
        } else {
            debugs(33, 3, "Continue with returning the error: " << requestError);
        }
    }

    if (context) {
        conn->quitAfterError(request);
        clientStreamNode *node = context->getClientReplyContext();
        clientReplyContext *repContext = dynamic_cast<clientReplyContext *>(node->data.getRaw());
        assert (repContext);

        repContext->setReplyToError(requestError, errStatusCode, method, context->http->uri, conn->clientConnection->remote, NULL, requestErrorBytes, NULL);

        assert(context->http->out.offset == 0);
        context->pullData();
    } // else Probably an ERR_REQUEST_START_TIMEOUT error so just return.
    return false;
}

void
clientProcessRequestFinished(ConnStateData *conn, const HttpRequest::Pointer &request)
{
    /*
     * DPW 2007-05-18
     * Moved the TCP_RESET feature from clientReplyContext::sendMoreData
     * to here because calling comm_reset_close() causes http to
     * be freed before accessing.
     */
    if (request != NULL && request->flags.resetTcp && Comm::IsConnOpen(conn->clientConnection)) {
        debugs(33, 3, HERE << "Sending TCP RST on " << conn->clientConnection);
        conn->flags.readMore = false;
        comm_reset_close(conn->clientConnection);
    }
}

void
clientProcessRequest(ConnStateData *conn, const Http1::RequestParserPointer &hp, ClientSocketContext *context)
{
    ClientHttpRequest *http = context->http;
    bool chunked = false;
    bool mustReplyToOptions = false;
    bool unsupportedTe = false;
    bool expectBody = false;

    // We already have the request parsed and checked, so we
    // only need to go through the final body/conn setup to doCallouts().
    assert(http->request);
    HttpRequest::Pointer request = http->request;

    // temporary hack to avoid splitting this huge function with sensitive code
    const bool isFtp = !hp;

    // Some blobs below are still HTTP-specific, but we would have to rewrite
    // this entire function to remove them from the FTP code path. Connection
    // setup and body_pipe preparation blobs are needed for FTP.

    request->clientConnectionManager = conn;

    request->flags.accelerated = http->flags.accel;
    request->flags.sslBumped=conn->switchedToHttps();
    request->flags.ignoreCc = conn->port->ignore_cc;
    // TODO: decouple http->flags.accel from request->flags.sslBumped
    request->flags.noDirect = (request->flags.accelerated && !request->flags.sslBumped) ?
                              !conn->port->allow_direct : 0;
#if USE_AUTH
    if (request->flags.sslBumped) {
        if (conn->getAuth() != NULL)
            request->auth_user_request = conn->getAuth();
    }
#endif

    /** \par
     * If transparent or interception mode is working clone the transparent and interception flags
     * from the port settings to the request.
     */
    if (http->clientConnection != NULL) {
        request->flags.intercepted = ((http->clientConnection->flags & COMM_INTERCEPTION) != 0);
        request->flags.interceptTproxy = ((http->clientConnection->flags & COMM_TRANSPARENT) != 0 ) ;
        static const bool proxyProtocolPort = (conn->port != NULL) ? conn->port->flags.proxySurrogate : false;
        if (request->flags.interceptTproxy && !proxyProtocolPort) {
            if (Config.accessList.spoof_client_ip) {
                ACLFilledChecklist *checklist = clientAclChecklistCreate(Config.accessList.spoof_client_ip, http);
                request->flags.spoofClientIp = (checklist->fastCheck() == ACCESS_ALLOWED);
                delete checklist;
            } else
                request->flags.spoofClientIp = true;
        } else
            request->flags.spoofClientIp = false;
    }

    if (internalCheck(request->urlpath.termedBuf())) {
        if (internalHostnameIs(request->GetHost()) && request->port == getMyPort()) {
            debugs(33, 2, "internal URL found: " << request->url.getScheme() << "://" << request->GetHost() <<
                   ':' << request->port);
            http->flags.internal = true;
        } else if (Config.onoff.global_internal_static && internalStaticCheck(request->urlpath.termedBuf())) {
            debugs(33, 2, "internal URL found: " << request->url.getScheme() << "://" << request->GetHost() <<
                   ':' << request->port << " (global_internal_static on)");
            request->SetHost(internalHostname());
            request->port = getMyPort();
            http->flags.internal = true;
        } else
            debugs(33, 2, "internal URL found: " << request->url.getScheme() << "://" << request->GetHost() <<
                   ':' << request->port << " (not this proxy)");
    }

    request->flags.internal = http->flags.internal;
    setLogUri (http, urlCanonicalClean(request.getRaw()));
    request->client_addr = conn->clientConnection->remote; // XXX: remove reuest->client_addr member.
#if FOLLOW_X_FORWARDED_FOR
    // indirect client gets stored here because it is an HTTP header result (from X-Forwarded-For:)
    // not a details about teh TCP connection itself
    request->indirect_client_addr = conn->clientConnection->remote;
#endif /* FOLLOW_X_FORWARDED_FOR */
    request->my_addr = conn->clientConnection->local;
    request->myportname = conn->port->name;

    if (!isFtp) {
        // XXX: for non-HTTP messages instantiate a different HttpMsg child type
        // for now Squid only supports HTTP requests
        const AnyP::ProtocolVersion &http_ver = hp->messageProtocol();
        assert(request->http_ver.protocol == http_ver.protocol);
        request->http_ver.major = http_ver.major;
        request->http_ver.minor = http_ver.minor;
    }

    // Link this HttpRequest to ConnStateData relatively early so the following complex handling can use it
    // TODO: this effectively obsoletes a lot of conn->FOO copying. That needs cleaning up later.
    request->clientConnectionManager = conn;

    if (request->header.chunked()) {
        chunked = true;
    } else if (request->header.has(HDR_TRANSFER_ENCODING)) {
        const String te = request->header.getList(HDR_TRANSFER_ENCODING);
        // HTTP/1.1 requires chunking to be the last encoding if there is one
        unsupportedTe = te.size() && te != "identity";
    } // else implied identity coding

    mustReplyToOptions = (request->method == Http::METHOD_OPTIONS) &&
                         (request->header.getInt64(HDR_MAX_FORWARDS) == 0);
    if (!urlCheckRequest(request.getRaw()) || mustReplyToOptions || unsupportedTe) {
        clientStreamNode *node = context->getClientReplyContext();
        conn->quitAfterError(request.getRaw());
        clientReplyContext *repContext = dynamic_cast<clientReplyContext *>(node->data.getRaw());
        assert (repContext);
        repContext->setReplyToError(ERR_UNSUP_REQ, Http::scNotImplemented, request->method, NULL,
                                    conn->clientConnection->remote, request.getRaw(), NULL, NULL);
        assert(context->http->out.offset == 0);
        context->pullData();
        clientProcessRequestFinished(conn, request);
        return;
    }

    if (!chunked && !clientIsContentLengthValid(request.getRaw())) {
        clientStreamNode *node = context->getClientReplyContext();
        clientReplyContext *repContext = dynamic_cast<clientReplyContext *>(node->data.getRaw());
        assert (repContext);
        conn->quitAfterError(request.getRaw());
        repContext->setReplyToError(ERR_INVALID_REQ,
                                    Http::scLengthRequired, request->method, NULL,
                                    conn->clientConnection->remote, request.getRaw(), NULL, NULL);
        assert(context->http->out.offset == 0);
        context->pullData();
        clientProcessRequestFinished(conn, request);
        return;
    }

    clientSetKeepaliveFlag(http);
    // Let tunneling code be fully responsible for CONNECT requests
    if (http->request->method == Http::METHOD_CONNECT) {
        context->mayUseConnection(true);
        conn->flags.readMore = false;
    }

#if USE_OPENSSL
    if (conn->switchedToHttps() && conn->serveDelayedError(context)) {
        clientProcessRequestFinished(conn, request);
        return;
    }
#endif

    /* Do we expect a request-body? */
    expectBody = chunked || request->content_length > 0;
    if (!context->mayUseConnection() && expectBody) {
        request->body_pipe = conn->expectRequestBody(
                                 chunked ? -1 : request->content_length);

        /* Is it too large? */
        if (!chunked && // if chunked, we will check as we accumulate
                clientIsRequestBodyTooLargeForPolicy(request->content_length)) {
            clientStreamNode *node = context->getClientReplyContext();
            clientReplyContext *repContext = dynamic_cast<clientReplyContext *>(node->data.getRaw());
            assert (repContext);
            conn->quitAfterError(request.getRaw());
            repContext->setReplyToError(ERR_TOO_BIG,
                                        Http::scPayloadTooLarge, Http::METHOD_NONE, NULL,
                                        conn->clientConnection->remote, http->request, NULL, NULL);
            assert(context->http->out.offset == 0);
            context->pullData();
            clientProcessRequestFinished(conn, request);
            return;
        }

        if (!isFtp) {
            // We may stop producing, comm_close, and/or call setReplyToError()
            // below, so quit on errors to avoid http->doCallouts()
            if (!conn->handleRequestBodyData()) {
                clientProcessRequestFinished(conn, request);
                return;
            }

            if (!request->body_pipe->productionEnded()) {
                debugs(33, 5, "need more request body");
                context->mayUseConnection(true);
                assert(conn->flags.readMore);
            }
        }
    }

    http->calloutContext = new ClientRequestContext(http);

    http->doCallouts();

    clientProcessRequestFinished(conn, request);
}

int
ConnStateData::pipelinePrefetchMax() const
{
    return Config.pipeline_max_prefetch;
}

/**
 * Limit the number of concurrent requests.
 * \return true  when there are available position(s) in the pipeline queue for another request.
 * \return false when the pipeline queue is full or disabled.
 */
bool
ConnStateData::concurrentRequestQueueFilled() const
{
    const int existingRequestCount = getConcurrentRequestCount();

    // default to the configured pipeline size.
    // add 1 because the head of pipeline is counted in concurrent requests and not prefetch queue
#if USE_OPENSSL
    const int internalRequest = (transparent() && sslBumpMode == Ssl::bumpSplice) ? 1 : 0;
#else
    const int internalRequest = 0;
#endif
    const int concurrentRequestLimit = pipelinePrefetchMax() + 1 + internalRequest;

    // when queue filled already we cant add more.
    if (existingRequestCount >= concurrentRequestLimit) {
        debugs(33, 3, clientConnection << " max concurrent requests reached (" << concurrentRequestLimit << ")");
        debugs(33, 5, clientConnection << " deferring new request until one is done");
        return true;
    }

    return false;
}

/**
 * Perform proxy_protocol_access ACL tests on the client which
 * connected to PROXY protocol port to see if we trust the
 * sender enough to accept their PROXY header claim.
 */
bool
ConnStateData::proxyProtocolValidateClient()
{
    if (!Config.accessList.proxyProtocol)
        return proxyProtocolError("PROXY client not permitted by default ACL");

    ACLFilledChecklist ch(Config.accessList.proxyProtocol, NULL, clientConnection->rfc931);
    ch.src_addr = clientConnection->remote;
    ch.my_addr = clientConnection->local;
    ch.conn(this);

    if (ch.fastCheck() != ACCESS_ALLOWED)
        return proxyProtocolError("PROXY client not permitted by ACLs");

    return true;
}

/**
 * Perform cleanup on PROXY protocol errors.
 * If header parsing hits a fatal error terminate the connection,
 * otherwise wait for more data.
 */
bool
ConnStateData::proxyProtocolError(const char *msg)
{
    if (msg) {
        // This is important to know, but maybe not so much that flooding the log is okay.
#if QUIET_PROXY_PROTOCOL
        // display the first of every 32 occurances at level 1, the others at level 2.
        static uint8_t hide = 0;
        debugs(33, (hide++ % 32 == 0 ? DBG_IMPORTANT : 2), msg << " from " << clientConnection);
#else
        debugs(33, DBG_IMPORTANT, msg << " from " << clientConnection);
#endif
        mustStop(msg);
    }
    return false;
}

/// magic octet prefix for PROXY protocol version 1
static const SBuf Proxy1p0magic("PROXY ", 6);

/// magic octet prefix for PROXY protocol version 2
static const SBuf Proxy2p0magic("\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A", 12);

/**
 * Test the connection read buffer for PROXY protocol header.
 * Version 1 and 2 header currently supported.
 */
bool
ConnStateData::parseProxyProtocolHeader()
{
    // http://www.haproxy.org/download/1.5/doc/proxy-protocol.txt

    // detect and parse PROXY/2.0 protocol header
    if (in.buf.startsWith(Proxy2p0magic))
        return parseProxy2p0();

    // detect and parse PROXY/1.0 protocol header
    if (in.buf.startsWith(Proxy1p0magic))
        return parseProxy1p0();

    // detect and terminate other protocols
    if (in.buf.length() >= Proxy2p0magic.length()) {
        // PROXY/1.0 magic is shorter, so we know that
        // the input does not start with any PROXY magic
        return proxyProtocolError("PROXY protocol error: invalid header");
    }

    // TODO: detect short non-magic prefixes earlier to avoid
    // waiting for more data which may never come

    // not enough bytes to parse yet.
    return false;
}

/// parse the PROXY/1.0 protocol header from the connection read buffer
bool
ConnStateData::parseProxy1p0()
{
    ::Parser::Tokenizer tok(in.buf);
    tok.skip(Proxy1p0magic);

    // skip to first LF (assumes it is part of CRLF)
    static const CharacterSet lineContent = CharacterSet::LF.complement("non-LF");
    SBuf line;
    if (tok.prefix(line, lineContent, 107-Proxy1p0magic.length())) {
        if (tok.skip('\n')) {
            // found valid header
            in.buf = tok.remaining();
            needProxyProtocolHeader_ = false;
            // reset the tokenizer to work on found line only.
            tok.reset(line);
        } else
            return false; // no LF yet

    } else // protocol error only if there are more than 107 bytes prefix header
        return proxyProtocolError(in.buf.length() > 107? "PROXY/1.0 error: missing CRLF" : NULL);

    static const SBuf unknown("UNKNOWN"), tcpName("TCP");
    if (tok.skip(tcpName)) {

        // skip TCP/IP version number
        static const CharacterSet tcpVersions("TCP-version","46");
        if (!tok.skipOne(tcpVersions))
            return proxyProtocolError("PROXY/1.0 error: missing TCP version");

        // skip SP after protocol version
        if (!tok.skip(' '))
            return proxyProtocolError("PROXY/1.0 error: missing SP");

        SBuf ipa, ipb;
        int64_t porta, portb;
        static const CharacterSet ipChars = CharacterSet("IP Address",".:") + CharacterSet::HEXDIG;

        // parse:  src-IP SP dst-IP SP src-port SP dst-port CR
        // leave the LF until later.
        const bool correct = tok.prefix(ipa, ipChars) && tok.skip(' ') &&
                             tok.prefix(ipb, ipChars) && tok.skip(' ') &&
                             tok.int64(porta) && tok.skip(' ') &&
                             tok.int64(portb) &&
                             tok.skip('\r');
        if (!correct)
            return proxyProtocolError("PROXY/1.0 error: invalid syntax");

        // parse IP and port strings
        Ip::Address originalClient, originalDest;

        if (!originalClient.GetHostByName(ipa.c_str()))
            return proxyProtocolError("PROXY/1.0 error: invalid src-IP address");

        if (!originalDest.GetHostByName(ipb.c_str()))
            return proxyProtocolError("PROXY/1.0 error: invalid dst-IP address");

        if (porta > 0 && porta <= 0xFFFF) // max uint16_t
            originalClient.port(static_cast<uint16_t>(porta));
        else
            return proxyProtocolError("PROXY/1.0 error: invalid src port");

        if (portb > 0 && portb <= 0xFFFF) // max uint16_t
            originalDest.port(static_cast<uint16_t>(portb));
        else
            return proxyProtocolError("PROXY/1.0 error: invalid dst port");

        // we have original client and destination details now
        // replace the client connection values
        debugs(33, 5, "PROXY/1.0 protocol on connection " << clientConnection);
        clientConnection->local = originalDest;
        clientConnection->remote = originalClient;
        clientConnection->flags ^= COMM_TRANSPARENT; // prevent TPROXY spoofing of this new IP.
        debugs(33, 5, "PROXY/1.0 upgrade: " << clientConnection);

        // repeat fetch ensuring the new client FQDN can be logged
        if (Config.onoff.log_fqdn)
            fqdncache_gethostbyaddr(clientConnection->remote, FQDN_LOOKUP_IF_MISS);

        return true;

    } else if (tok.skip(unknown)) {
        // found valid but unusable header
        return true;

    } else
        return proxyProtocolError("PROXY/1.0 error: invalid protocol family");

    return false;
}

/// parse the PROXY/2.0 protocol header from the connection read buffer
bool
ConnStateData::parseProxy2p0()
{
    static const SBuf::size_type prefixLen = Proxy2p0magic.length();
    if (in.buf.length() < prefixLen + 4)
        return false; // need more bytes

    if ((in.buf[prefixLen] & 0xF0) != 0x20) // version == 2 is mandatory
        return proxyProtocolError("PROXY/2.0 error: invalid version");

    const char command = (in.buf[prefixLen] & 0x0F);
    if ((command & 0xFE) != 0x00) // values other than 0x0-0x1 are invalid
        return proxyProtocolError("PROXY/2.0 error: invalid command");

    const char family = (in.buf[prefixLen+1] & 0xF0) >>4;
    if (family > 0x3) // values other than 0x0-0x3 are invalid
        return proxyProtocolError("PROXY/2.0 error: invalid family");

    const char proto = (in.buf[prefixLen+1] & 0x0F);
    if (proto > 0x2) // values other than 0x0-0x2 are invalid
        return proxyProtocolError("PROXY/2.0 error: invalid protocol type");

    const char *clen = in.buf.rawContent() + prefixLen + 2;
    uint16_t len;
    memcpy(&len, clen, sizeof(len));
    len = ntohs(len);

    if (in.buf.length() < prefixLen + 4 + len)
        return false; // need more bytes

    in.buf.consume(prefixLen + 4); // 4 being the extra bytes
    const SBuf extra = in.buf.consume(len);
    needProxyProtocolHeader_ = false; // found successfully

    // LOCAL connections do nothing with the extras
    if (command == 0x00/* LOCAL*/)
        return true;

    union pax {
        struct {        /* for TCP/UDP over IPv4, len = 12 */
            struct in_addr src_addr;
            struct in_addr dst_addr;
            uint16_t src_port;
            uint16_t dst_port;
        } ipv4_addr;
        struct {        /* for TCP/UDP over IPv6, len = 36 */
            struct in6_addr src_addr;
            struct in6_addr dst_addr;
            uint16_t src_port;
            uint16_t dst_port;
        } ipv6_addr;
#if NOT_SUPPORTED
        struct {        /* for AF_UNIX sockets, len = 216 */
            uint8_t src_addr[108];
            uint8_t dst_addr[108];
        } unix_addr;
#endif
    };

    pax ipu;
    memcpy(&ipu, extra.rawContent(), sizeof(pax));

    // replace the client connection values
    debugs(33, 5, "PROXY/2.0 protocol on connection " << clientConnection);
    switch (family) {
    case 0x1: // IPv4
        clientConnection->local = ipu.ipv4_addr.dst_addr;
        clientConnection->local.port(ntohs(ipu.ipv4_addr.dst_port));
        clientConnection->remote = ipu.ipv4_addr.src_addr;
        clientConnection->remote.port(ntohs(ipu.ipv4_addr.src_port));
        clientConnection->flags ^= COMM_TRANSPARENT; // prevent TPROXY spoofing of this new IP.
        break;
    case 0x2: // IPv6
        clientConnection->local = ipu.ipv6_addr.dst_addr;
        clientConnection->local.port(ntohs(ipu.ipv6_addr.dst_port));
        clientConnection->remote = ipu.ipv6_addr.src_addr;
        clientConnection->remote.port(ntohs(ipu.ipv6_addr.src_port));
        clientConnection->flags ^= COMM_TRANSPARENT; // prevent TPROXY spoofing of this new IP.
        break;
    default: // do nothing
        break;
    }
    debugs(33, 5, "PROXY/2.0 upgrade: " << clientConnection);

    // repeat fetch ensuring the new client FQDN can be logged
    if (Config.onoff.log_fqdn)
        fqdncache_gethostbyaddr(clientConnection->remote, FQDN_LOOKUP_IF_MISS);

    return true;
}

void
ConnStateData::receivedFirstByte()
{
    if (receivedFirstByte_)
        return;

    receivedFirstByte_ = true;
    // Set timeout to Config.Timeout.request
    typedef CommCbMemFunT<ConnStateData, CommTimeoutCbParams> TimeoutDialer;
    AsyncCall::Pointer timeoutCall =  JobCallback(33, 5,
                                      TimeoutDialer, this, ConnStateData::requestTimeout);
    commSetConnTimeout(clientConnection, Config.Timeout.request, timeoutCall);
}

/**
 * Attempt to parse one or more requests from the input buffer.
 * Returns true after completing parsing of at least one request [header]. That
 * includes cases where parsing ended with an error (e.g., a huge request).
 */
bool
ConnStateData::clientParseRequests()
{
    bool parsed_req = false;

    debugs(33, 5, HERE << clientConnection << ": attempting to parse");

    // Loop while we have read bytes that are not needed for producing the body
    // On errors, bodyPipe may become nil, but readMore will be cleared
    while (!in.buf.isEmpty() && !bodyPipe && flags.readMore) {

        /* Don't try to parse if the buffer is empty */
        if (in.buf.isEmpty())
            break;

        /* Limit the number of concurrent requests */
        if (concurrentRequestQueueFilled())
            break;

        /*Do not read more requests if persistent connection lifetime exceeded*/
        if (Config.Timeout.pconnLifetime && clientConnection->lifeTime() > Config.Timeout.pconnLifetime) {
            flags.readMore = false;
            break;
        }

        // try to parse the PROXY protocol header magic bytes
        if (needProxyProtocolHeader_ && !parseProxyProtocolHeader())
            break;

        if (ClientSocketContext *context = parseOneRequest()) {
            debugs(33, 5, clientConnection << ": done parsing a request");

            AsyncCall::Pointer timeoutCall = commCbCall(5, 4, "clientLifetimeTimeout",
                                             CommTimeoutCbPtrFun(clientLifetimeTimeout, context->http));
            commSetConnTimeout(clientConnection, Config.Timeout.lifetime, timeoutCall);

            context->registerWithConn();

            processParsedRequest(context);

            parsed_req = true; // XXX: do we really need to parse everything right NOW ?

            if (context->mayUseConnection()) {
                debugs(33, 3, HERE << "Not parsing new requests, as this request may need the connection");
                break;
            }
        } else {
            debugs(33, 5, clientConnection << ": not enough request data: " <<
                   in.buf.length() << " < " << Config.maxRequestHeaderSize);
            Must(in.buf.length() < Config.maxRequestHeaderSize);
            break;
        }
    }

    /* XXX where to 'finish' the parsing pass? */
    return parsed_req;
}

void
ConnStateData::clientReadRequest(const CommIoCbParams &io)
{
    debugs(33,5, io.conn);
    Must(reading());
    reader = NULL;

    /* Bail out quickly on Comm::ERR_CLOSING - close handlers will tidy up */
    if (io.flag == Comm::ERR_CLOSING) {
        debugs(33,5, io.conn << " closing Bailout.");
        return;
    }

    assert(Comm::IsConnOpen(clientConnection));
    assert(io.conn->fd == clientConnection->fd);

    /*
     * Don't reset the timeout value here. The value should be
     * counting Config.Timeout.request and applies to the request
     * as a whole, not individual read() calls.
     * Plus, it breaks our lame *HalfClosed() detection
     */

    in.maybeMakeSpaceAvailable();
    CommIoCbParams rd(this); // will be expanded with ReadNow results
    rd.conn = io.conn;
    switch (Comm::ReadNow(rd, in.buf)) {
    case Comm::INPROGRESS:
        if (in.buf.isEmpty())
            debugs(33, 2, io.conn << ": no data to process, " << xstrerr(rd.xerrno));
        readSomeData();
        return;

    case Comm::OK:
        kb_incr(&(statCounter.client_http.kbytes_in), rd.size);
        if (!receivedFirstByte_)
            receivedFirstByte();
        // may comm_close or setReplyToError
        if (!handleReadData())
            return;

        /* Continue to process previously read data */
        break;

    case Comm::ENDFILE: // close detected by 0-byte read
        debugs(33, 5, io.conn << " closed?");

        if (connFinishedWithConn(rd.size)) {
            clientConnection->close();
            return;
        }

        /* It might be half-closed, we can't tell */
        fd_table[io.conn->fd].flags.socket_eof = true;
        commMarkHalfClosed(io.conn->fd);
        fd_note(io.conn->fd, "half-closed");

        /* There is one more close check at the end, to detect aborted
         * (partial) requests. At this point we can't tell if the request
         * is partial.
         */

        /* Continue to process previously read data */
        break;

    // case Comm::COMM_ERROR:
    default: // no other flags should ever occur
        debugs(33, 2, io.conn << ": got flag " << rd.flag << "; " << xstrerr(rd.xerrno));
        notifyAllContexts(rd.xerrno);
        io.conn->close();
        return;
    }

    /* Process next request */
    if (getConcurrentRequestCount() == 0)
        fd_note(io.fd, "Reading next request");

    if (!clientParseRequests()) {
        if (!isOpen())
            return;
        /*
         * If the client here is half closed and we failed
         * to parse a request, close the connection.
         * The above check with connFinishedWithConn() only
         * succeeds _if_ the buffer is empty which it won't
         * be if we have an incomplete request.
         * XXX: This duplicates ClientSocketContext::keepaliveNextRequest
         */
        if (getConcurrentRequestCount() == 0 && commIsHalfClosed(io.fd)) {
            debugs(33, 5, HERE << io.conn << ": half-closed connection, no completed request parsed, connection closing.");
            clientConnection->close();
            return;
        }
    }

    if (!isOpen())
        return;

    clientAfterReadingRequests();
}

/**
 * called when new request data has been read from the socket
 *
 * \retval false called comm_close or setReplyToError (the caller should bail)
 * \retval true  we did not call comm_close or setReplyToError
 */
bool
ConnStateData::handleReadData()
{
    // if we are reading a body, stuff data into the body pipe
    if (bodyPipe != NULL)
        return handleRequestBodyData();
    return true;
}

/**
 * called when new request body data has been buffered in in.buf
 * may close the connection if we were closing and piped everything out
 *
 * \retval false called comm_close or setReplyToError (the caller should bail)
 * \retval true  we did not call comm_close or setReplyToError
 */
bool
ConnStateData::handleRequestBodyData()
{
    assert(bodyPipe != NULL);

    size_t putSize = 0;

    if (in.bodyParser) { // chunked encoding
        if (const err_type error = handleChunkedRequestBody(putSize)) {
            abortChunkedRequestBody(error);
            return false;
        }
    } else { // identity encoding
        debugs(33,5, HERE << "handling plain request body for " << clientConnection);
        putSize = bodyPipe->putMoreData(in.buf.c_str(), in.buf.length());
        if (!bodyPipe->mayNeedMoreData()) {
            // BodyPipe will clear us automagically when we produced everything
            bodyPipe = NULL;
        }
    }

    if (putSize > 0)
        consumeInput(putSize);

    if (!bodyPipe) {
        debugs(33,5, HERE << "produced entire request body for " << clientConnection);

        if (const char *reason = stoppedSending()) {
            /* we've finished reading like good clients,
             * now do the close that initiateClose initiated.
             */
            debugs(33, 3, HERE << "closing for earlier sending error: " << reason);
            clientConnection->close();
            return false;
        }
    }

    return true;
}

/// parses available chunked encoded body bytes, checks size, returns errors
err_type
ConnStateData::handleChunkedRequestBody(size_t &putSize)
{
    debugs(33, 7, "chunked from " << clientConnection << ": " << in.buf.length());

    try { // the parser will throw on errors

        if (in.buf.isEmpty()) // nothing to do
            return ERR_NONE;

        MemBuf raw; // ChunkedCodingParser only works with MemBufs
        // add one because MemBuf will assert if it cannot 0-terminate
        raw.init(in.buf.length(), in.buf.length()+1);
        raw.append(in.buf.c_str(), in.buf.length());

        const mb_size_t wasContentSize = raw.contentSize();
        BodyPipeCheckout bpc(*bodyPipe);
        const bool parsed = in.bodyParser->parse(&raw, &bpc.buf);
        bpc.checkIn();
        putSize = wasContentSize - raw.contentSize();

        // dechunk then check: the size limit applies to _dechunked_ content
        if (clientIsRequestBodyTooLargeForPolicy(bodyPipe->producedSize()))
            return ERR_TOO_BIG;

        if (parsed) {
            finishDechunkingRequest(true);
            Must(!bodyPipe);
            return ERR_NONE; // nil bodyPipe implies body end for the caller
        }

        // if chunk parser needs data, then the body pipe must need it too
        Must(!in.bodyParser->needsMoreData() || bodyPipe->mayNeedMoreData());

        // if parser needs more space and we can consume nothing, we will stall
        Must(!in.bodyParser->needsMoreSpace() || bodyPipe->buf().hasContent());
    } catch (...) { // TODO: be more specific
        debugs(33, 3, HERE << "malformed chunks" << bodyPipe->status());
        return ERR_INVALID_REQ;
    }

    debugs(33, 7, HERE << "need more chunked data" << *bodyPipe->status());
    return ERR_NONE;
}

/// quit on errors related to chunked request body handling
void
ConnStateData::abortChunkedRequestBody(const err_type error)
{
    finishDechunkingRequest(false);

    // XXX: The code below works if we fail during initial request parsing,
    // but if we fail when the server connection is used already, the server may send
    // us its response too, causing various assertions. How to prevent that?
#if WE_KNOW_HOW_TO_SEND_ERRORS
    ClientSocketContext::Pointer context = getCurrentContext();
    if (context != NULL && !context->http->out.offset) { // output nothing yet
        clientStreamNode *node = context->getClientReplyContext();
        clientReplyContext *repContext = dynamic_cast<clientReplyContext*>(node->data.getRaw());
        assert(repContext);
        const Http::StatusCode scode = (error == ERR_TOO_BIG) ?
                                       Http::scPayloadTooLarge : HTTP_BAD_REQUEST;
        repContext->setReplyToError(error, scode,
                                    repContext->http->request->method,
                                    repContext->http->uri,
                                    CachePeer,
                                    repContext->http->request,
                                    in.buf, NULL);
        context->pullData();
    } else {
        // close or otherwise we may get stuck as nobody will notice the error?
        comm_reset_close(clientConnection);
    }
#else
    debugs(33, 3, HERE << "aborting chunked request without error " << error);
    comm_reset_close(clientConnection);
#endif
    flags.readMore = false;
}

void
ConnStateData::noteBodyConsumerAborted(BodyPipe::Pointer )
{
    // request reader may get stuck waiting for space if nobody consumes body
    if (bodyPipe != NULL)
        bodyPipe->enableAutoConsumption();

    // kids extend
}

/** general lifetime handler for HTTP requests */
void
ConnStateData::requestTimeout(const CommTimeoutCbParams &io)
{
    if (Config.accessList.on_unsupported_protocol && !receivedFirstByte_) {
#if USE_OPENSSL
        if (serverBump() && (serverBump()->act.step1 == Ssl::bumpPeek || serverBump()->act.step1 == Ssl::bumpStare)) {
            if (spliceOnError(ERR_REQUEST_START_TIMEOUT)) {
                receivedFirstByte();
                return;
            }
        } else if (fd_table[io.conn->fd].ssl == NULL)
#endif
        {
            const HttpRequestMethod method;
            if (clientTunnelOnError(this, NULL, NULL, method, ERR_REQUEST_START_TIMEOUT, Http::scNone, NULL)) {
                // Tunnel established. Set receivedFirstByte to avoid loop.
                receivedFirstByte();
                return;
            }
        }
    }
    /*
    * Just close the connection to not confuse browsers
    * using persistent connections. Some browsers open
    * a connection and then do not use it until much
    * later (presumeably because the request triggering
    * the open has already been completed on another
    * connection)
    */
    debugs(33, 3, "requestTimeout: FD " << io.fd << ": lifetime is expired.");
    io.conn->close();
}

static void
clientLifetimeTimeout(const CommTimeoutCbParams &io)
{
    ClientHttpRequest *http = static_cast<ClientHttpRequest *>(io.data);
    debugs(33, DBG_IMPORTANT, "WARNING: Closing client connection due to lifetime timeout");
    debugs(33, DBG_IMPORTANT, "\t" << http->uri);
    http->al->http.timedout = true;
    if (Comm::IsConnOpen(io.conn))
        io.conn->close();
}

ConnStateData::ConnStateData(const MasterXaction::Pointer &xact) :
    AsyncJob("ConnStateData"), // kids overwrite
    nrequests(0),
#if USE_OPENSSL
    sslBumpMode(Ssl::bumpEnd),
#endif
    needProxyProtocolHeader_(false),
#if USE_OPENSSL
    switchedToHttps_(false),
    sslServerBump(NULL),
    signAlgorithm(Ssl::algSignTrusted),
#endif
    stoppedSending_(NULL),
    stoppedReceiving_(NULL),
    receivedFirstByte_(false)
{
    flags.readMore = true; // kids may overwrite
    flags.swanSang = false;

    pinning.host = NULL;
    pinning.port = -1;
    pinning.pinned = false;
    pinning.auth = false;
    pinning.zeroReply = false;
    pinning.peer = NULL;

    // store the details required for creating more MasterXaction objects as new requests come in
    clientConnection = xact->tcpClient;
    port = xact->squidPort;
    transferProtocol = port->transport; // default to the *_port protocol= setting. may change later.
    log_addr = xact->tcpClient->remote;
    log_addr.applyMask(Config.Addrs.client_netmask);
}

void
ConnStateData::start()
{
    BodyProducer::start();
    HttpControlMsgSink::start();

    if (port->disable_pmtu_discovery != DISABLE_PMTU_OFF &&
            (transparent() || port->disable_pmtu_discovery == DISABLE_PMTU_ALWAYS)) {
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DONT)
        int i = IP_PMTUDISC_DONT;
        if (setsockopt(clientConnection->fd, SOL_IP, IP_MTU_DISCOVER, &i, sizeof(i)) < 0)
            debugs(33, 2, "WARNING: Path MTU discovery disabling failed on " << clientConnection << " : " << xstrerror());
#else
        static bool reported = false;

        if (!reported) {
            debugs(33, DBG_IMPORTANT, "NOTICE: Path MTU discovery disabling is not supported on your platform.");
            reported = true;
        }
#endif
    }

    typedef CommCbMemFunT<ConnStateData, CommCloseCbParams> Dialer;
    AsyncCall::Pointer call = JobCallback(33, 5, Dialer, this, ConnStateData::connStateClosed);
    comm_add_close_handler(clientConnection->fd, call);

    if (Config.onoff.log_fqdn)
        fqdncache_gethostbyaddr(clientConnection->remote, FQDN_LOOKUP_IF_MISS);

#if USE_IDENT
    if (Ident::TheConfig.identLookup) {
        ACLFilledChecklist identChecklist(Ident::TheConfig.identLookup, NULL, NULL);
        identChecklist.src_addr = clientConnection->remote;
        identChecklist.my_addr = clientConnection->local;
        if (identChecklist.fastCheck() == ACCESS_ALLOWED)
            Ident::Start(clientConnection, clientIdentDone, this);
    }
#endif

    clientdbEstablished(clientConnection->remote, 1);

    needProxyProtocolHeader_ = port->flags.proxySurrogate;
    if (needProxyProtocolHeader_) {
        if (!proxyProtocolValidateClient()) // will close the connection on failure
            return;
    }

#if USE_DELAY_POOLS
    fd_table[clientConnection->fd].clientInfo = NULL;

    if (Config.onoff.client_db) {
        /* it was said several times that client write limiter does not work if client_db is disabled */

        ClientDelayPools& pools(Config.ClientDelay.pools);
        ACLFilledChecklist ch(NULL, NULL, NULL);

        // TODO: we check early to limit error response bandwith but we
        // should recheck when we can honor delay_pool_uses_indirect
        // TODO: we should also pass the port details for myportname here.
        ch.src_addr = clientConnection->remote;
        ch.my_addr = clientConnection->local;

        for (unsigned int pool = 0; pool < pools.size(); ++pool) {

            /* pools require explicit 'allow' to assign a client into them */
            if (pools[pool].access) {
                ch.accessList = pools[pool].access;
                allow_t answer = ch.fastCheck();
                if (answer == ACCESS_ALLOWED) {

                    /*  request client information from db after we did all checks
                        this will save hash lookup if client failed checks */
                    ClientInfo * cli = clientdbGetInfo(clientConnection->remote);
                    assert(cli);

                    /* put client info in FDE */
                    fd_table[clientConnection->fd].clientInfo = cli;

                    /* setup write limiter for this request */
                    const double burst = floor(0.5 +
                                               (pools[pool].highwatermark * Config.ClientDelay.initial)/100.0);
                    cli->setWriteLimiter(pools[pool].rate, burst, pools[pool].highwatermark);
                    break;
                } else {
                    debugs(83, 4, HERE << "Delay pool " << pool << " skipped because ACL " << answer);
                }
            }
        }
    }
#endif

    // kids must extend to actually start doing something (e.g., reading)
}

/** Handle a new connection on an HTTP socket. */
void
httpAccept(const CommAcceptCbParams &params)
{
    MasterXaction::Pointer xact = params.xaction;
    AnyP::PortCfgPointer s = xact->squidPort;

    // NP: it is possible the port was reconfigured when the call or accept() was queued.

    if (params.flag != Comm::OK) {
        // Its possible the call was still queued when the client disconnected
        debugs(33, 2, s->listenConn << ": accept failure: " << xstrerr(params.xerrno));
        return;
    }

    debugs(33, 4, params.conn << ": accepted");
    fd_note(params.conn->fd, "client http connect");

    if (s->tcp_keepalive.enabled)
        commSetTcpKeepalive(params.conn->fd, s->tcp_keepalive.idle, s->tcp_keepalive.interval, s->tcp_keepalive.timeout);

    ++incoming_sockets_accepted;

    // Socket is ready, setup the connection manager to start using it
    ConnStateData *connState = Http::NewServer(xact);
    AsyncJob::Start(connState); // usually async-calls readSomeData()
}

#if USE_OPENSSL

/** Create SSL connection structure and update fd_table */
static SSL *
httpsCreate(const Comm::ConnectionPointer &conn, SSL_CTX *sslContext)
{
    if (SSL *ssl = Ssl::CreateServer(sslContext, conn->fd, "client https start")) {
        debugs(33, 5, "will negotate SSL on " << conn);
        return ssl;
    }

    conn->close();
    return NULL;
}

/**
 *
 * \retval 1 on success
 * \retval 0 when needs more data
 * \retval -1 on error
 */
static int
Squid_SSL_accept(ConnStateData *conn, PF *callback)
{
    int fd = conn->clientConnection->fd;
    SSL *ssl = fd_table[fd].ssl;
    int ret;

    if ((ret = SSL_accept(ssl)) <= 0) {
        int ssl_error = SSL_get_error(ssl, ret);

        switch (ssl_error) {

        case SSL_ERROR_WANT_READ:
            Comm::SetSelect(fd, COMM_SELECT_READ, callback, conn, 0);
            return 0;

        case SSL_ERROR_WANT_WRITE:
            Comm::SetSelect(fd, COMM_SELECT_WRITE, callback, conn, 0);
            return 0;

        case SSL_ERROR_SYSCALL:

            if (ret == 0) {
                debugs(83, 2, "Error negotiating SSL connection on FD " << fd << ": Aborted by client: " << ssl_error);
            } else {
                int hard = 1;

                if (errno == ECONNRESET)
                    hard = 0;

                debugs(83, hard ? 1 : 2, "Error negotiating SSL connection on FD " <<
                       fd << ": " << strerror(errno) << " (" << errno << ")");
            }
            return -1;

        case SSL_ERROR_ZERO_RETURN:
            debugs(83, DBG_IMPORTANT, "Error negotiating SSL connection on FD " << fd << ": Closed by client");
            return -1;

        default:
            debugs(83, DBG_IMPORTANT, "Error negotiating SSL connection on FD " <<
                   fd << ": " << ERR_error_string(ERR_get_error(), NULL) <<
                   " (" << ssl_error << "/" << ret << ")");
            return -1;
        }

        /* NOTREACHED */
    }
    return 1;
}

/** negotiate an SSL connection */
static void
clientNegotiateSSL(int fd, void *data)
{
    ConnStateData *conn = (ConnStateData *)data;
    X509 *client_cert;
    SSL *ssl = fd_table[fd].ssl;

    int ret;
    if ((ret = Squid_SSL_accept(conn, clientNegotiateSSL)) <= 0) {
        if (ret < 0) // An error
            comm_close(fd);
        return;
    }

    if (SSL_session_reused(ssl)) {
        debugs(83, 2, "clientNegotiateSSL: Session " << SSL_get_session(ssl) <<
               " reused on FD " << fd << " (" << fd_table[fd].ipaddr << ":" << (int)fd_table[fd].remote_port << ")");
    } else {
        if (do_debug(83, 4)) {
            /* Write out the SSL session details.. actually the call below, but
             * OpenSSL headers do strange typecasts confusing GCC.. */
            /* PEM_write_SSL_SESSION(debug_log, SSL_get_session(ssl)); */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x00908000L
            PEM_ASN1_write((i2d_of_void *)i2d_SSL_SESSION, PEM_STRING_SSL_SESSION, debug_log, (char *)SSL_get_session(ssl), NULL,NULL,0,NULL,NULL);

#elif (ALLOW_ALWAYS_SSL_SESSION_DETAIL == 1)

            /* When using gcc 3.3.x and OpenSSL 0.9.7x sometimes a compile error can occur here.
            * This is caused by an unpredicatble gcc behaviour on a cast of the first argument
            * of PEM_ASN1_write(). For this reason this code section is disabled. To enable it,
            * define ALLOW_ALWAYS_SSL_SESSION_DETAIL=1.
            * Because there are two possible usable cast, if you get an error here, try the other
            * commented line. */

            PEM_ASN1_write((int(*)())i2d_SSL_SESSION, PEM_STRING_SSL_SESSION, debug_log, (char *)SSL_get_session(ssl), NULL,NULL,0,NULL,NULL);
            /* PEM_ASN1_write((int(*)(...))i2d_SSL_SESSION, PEM_STRING_SSL_SESSION, debug_log, (char *)SSL_get_session(ssl), NULL,NULL,0,NULL,NULL); */

#else

            debugs(83, 4, "With " OPENSSL_VERSION_TEXT ", session details are available only defining ALLOW_ALWAYS_SSL_SESSION_DETAIL=1 in the source." );

#endif
            /* Note: This does not automatically fflush the log file.. */
        }

        debugs(83, 2, "clientNegotiateSSL: New session " <<
               SSL_get_session(ssl) << " on FD " << fd << " (" <<
               fd_table[fd].ipaddr << ":" << (int)fd_table[fd].remote_port <<
               ")");
    }

    debugs(83, 3, "clientNegotiateSSL: FD " << fd << " negotiated cipher " <<
           SSL_get_cipher(ssl));

    client_cert = SSL_get_peer_certificate(ssl);

    if (client_cert != NULL) {
        debugs(83, 3, "clientNegotiateSSL: FD " << fd <<
               " client certificate: subject: " <<
               X509_NAME_oneline(X509_get_subject_name(client_cert), 0, 0));

        debugs(83, 3, "clientNegotiateSSL: FD " << fd <<
               " client certificate: issuer: " <<
               X509_NAME_oneline(X509_get_issuer_name(client_cert), 0, 0));

        X509_free(client_cert);
    } else {
        debugs(83, 5, "clientNegotiateSSL: FD " << fd <<
               " has no certificate.");
    }

    conn->readSomeData();
}

/**
 * If SSL_CTX is given, starts reading the SSL handshake.
 * Otherwise, calls switchToHttps to generate a dynamic SSL_CTX.
 */
static void
httpsEstablish(ConnStateData *connState,  SSL_CTX *sslContext)
{
    SSL *ssl = NULL;
    assert(connState);
    const Comm::ConnectionPointer &details = connState->clientConnection;

    if (!sslContext || !(ssl = httpsCreate(details, sslContext)))
        return;

    typedef CommCbMemFunT<ConnStateData, CommTimeoutCbParams> TimeoutDialer;
    AsyncCall::Pointer timeoutCall = JobCallback(33, 5, TimeoutDialer,
                                     connState, ConnStateData::requestTimeout);
    commSetConnTimeout(details, Config.Timeout.request, timeoutCall);

    Comm::SetSelect(details->fd, COMM_SELECT_READ, clientNegotiateSSL, connState, 0);
}

/**
 * A callback function to use with the ACLFilledChecklist callback.
 * In the case of ACCESS_ALLOWED answer initializes a bumped SSL connection,
 * else reverts the connection to tunnel mode.
 */
static void
httpsSslBumpAccessCheckDone(allow_t answer, void *data)
{
    ConnStateData *connState = (ConnStateData *) data;

    // if the connection is closed or closing, just return.
    if (!connState->isOpen())
        return;

    // Require both a match and a positive bump mode to work around exceptional
    // cases where ACL code may return ACCESS_ALLOWED with zero answer.kind.
    if (answer == ACCESS_ALLOWED && (answer.kind != Ssl::bumpNone && answer.kind != Ssl::bumpSplice)) {
        debugs(33, 2, "sslBump needed for " << connState->clientConnection << " method " << answer.kind);
        connState->sslBumpMode = static_cast<Ssl::BumpMode>(answer.kind);
    } else {
        debugs(33, 2, HERE << "sslBump not needed for " << connState->clientConnection);
        connState->sslBumpMode = Ssl::bumpNone;
    }

    // fake a CONNECT request to force connState to tunnel
    static char ip[MAX_IPSTRLEN];
    connState->clientConnection->local.toUrl(ip, sizeof(ip));
    // Pre-pend this fake request to the TLS bits already in the buffer
    SBuf retStr;
    retStr.append("CONNECT ").append(ip).append(" HTTP/1.1\r\nHost: ").append(ip).append("\r\n\r\n");
    connState->in.buf = retStr.append(connState->in.buf);
    bool ret = connState->handleReadData();
    if (ret)
        ret = connState->clientParseRequests();

    if (!ret) {
        debugs(33, 2, "Failed to start fake CONNECT request for SSL bumped connection: " << connState->clientConnection);
        connState->clientConnection->close();
    }
}

/** handle a new HTTPS connection */
static void
httpsAccept(const CommAcceptCbParams &params)
{
    MasterXaction::Pointer xact = params.xaction;
    const AnyP::PortCfgPointer s = xact->squidPort;

    // NP: it is possible the port was reconfigured when the call or accept() was queued.

    if (params.flag != Comm::OK) {
        // Its possible the call was still queued when the client disconnected
        debugs(33, 2, "httpsAccept: " << s->listenConn << ": accept failure: " << xstrerr(params.xerrno));
        return;
    }

    debugs(33, 4, HERE << params.conn << " accepted, starting SSL negotiation.");
    fd_note(params.conn->fd, "client https connect");

    if (s->tcp_keepalive.enabled) {
        commSetTcpKeepalive(params.conn->fd, s->tcp_keepalive.idle, s->tcp_keepalive.interval, s->tcp_keepalive.timeout);
    }

    ++incoming_sockets_accepted;

    // Socket is ready, setup the connection manager to start using it
    ConnStateData *connState = Https::NewServer(xact);
    AsyncJob::Start(connState); // usually async-calls postHttpsAccept()
}

void
ConnStateData::postHttpsAccept()
{
    if (port->flags.tunnelSslBumping) {
        debugs(33, 5, "accept transparent connection: " << clientConnection);

        if (!Config.accessList.ssl_bump) {
            httpsSslBumpAccessCheckDone(ACCESS_DENIED, this);
            return;
        }

        // Create a fake HTTP request for ssl_bump ACL check,
        // using tproxy/intercept provided destination IP and port.
        HttpRequest *request = new HttpRequest();
        static char ip[MAX_IPSTRLEN];
        assert(clientConnection->flags & (COMM_TRANSPARENT | COMM_INTERCEPTION));
        request->SetHost(clientConnection->local.toStr(ip, sizeof(ip)));
        request->port = clientConnection->local.port();
        request->myportname = port->name;

        ACLFilledChecklist *acl_checklist = new ACLFilledChecklist(Config.accessList.ssl_bump, request, NULL);
        acl_checklist->src_addr = clientConnection->remote;
        acl_checklist->my_addr = port->s;
        acl_checklist->nonBlockingCheck(httpsSslBumpAccessCheckDone, this);
        return;
    } else {
        SSL_CTX *sslContext = port->staticSslContext.get();
        httpsEstablish(this, sslContext);
    }
}

void
ConnStateData::sslCrtdHandleReplyWrapper(void *data, const Helper::Reply &reply)
{
    ConnStateData * state_data = (ConnStateData *)(data);
    state_data->sslCrtdHandleReply(reply);
}

void
ConnStateData::sslCrtdHandleReply(const Helper::Reply &reply)
{
    if (reply.result == Helper::BrokenHelper) {
        debugs(33, 5, HERE << "Certificate for " << sslConnectHostOrIp << " cannot be generated. ssl_crtd response: " << reply);
    } else if (!reply.other().hasContent()) {
        debugs(1, DBG_IMPORTANT, HERE << "\"ssl_crtd\" helper returned <NULL> reply.");
    } else {
        Ssl::CrtdMessage reply_message(Ssl::CrtdMessage::REPLY);
        if (reply_message.parse(reply.other().content(), reply.other().contentSize()) != Ssl::CrtdMessage::OK) {
            debugs(33, 5, HERE << "Reply from ssl_crtd for " << sslConnectHostOrIp << " is incorrect");
        } else {
            if (reply.result != Helper::Okay) {
                debugs(33, 5, HERE << "Certificate for " << sslConnectHostOrIp << " cannot be generated. ssl_crtd response: " << reply_message.getBody());
            } else {
                debugs(33, 5, HERE << "Certificate for " << sslConnectHostOrIp << " was successfully recieved from ssl_crtd");
                if (sslServerBump && (sslServerBump->act.step1 == Ssl::bumpPeek || sslServerBump->act.step1 == Ssl::bumpStare)) {
                    doPeekAndSpliceStep();
                    SSL *ssl = fd_table[clientConnection->fd].ssl;
                    bool ret = Ssl::configureSSLUsingPkeyAndCertFromMemory(ssl, reply_message.getBody().c_str(), *port);
                    if (!ret)
                        debugs(33, 5, "Failed to set certificates to ssl object for PeekAndSplice mode");
                } else {
                    SSL_CTX *ctx = Ssl::generateSslContextUsingPkeyAndCertFromMemory(reply_message.getBody().c_str(), *port);
                    getSslContextDone(ctx, true);
                }
                return;
            }
        }
    }
    getSslContextDone(NULL);
}

void ConnStateData::buildSslCertGenerationParams(Ssl::CertificateProperties &certProperties)
{
    certProperties.commonName =  sslCommonName.size() > 0 ? sslCommonName.termedBuf() : sslConnectHostOrIp.termedBuf();

    // fake certificate adaptation requires bump-server-first mode
    if (!sslServerBump) {
        assert(port->signingCert.get());
        certProperties.signWithX509.resetAndLock(port->signingCert.get());
        if (port->signPkey.get())
            certProperties.signWithPkey.resetAndLock(port->signPkey.get());
        certProperties.signAlgorithm = Ssl::algSignTrusted;
        return;
    }

    // In case of an error while connecting to the secure server, use a fake
    // trusted certificate, with no mimicked fields and no adaptation
    // algorithms. There is nothing we can mimic so we want to minimize the
    // number of warnings the user will have to see to get to the error page.
    assert(sslServerBump->entry);
    if (sslServerBump->entry->isEmpty()) {
        if (X509 *mimicCert = sslServerBump->serverCert.get())
            certProperties.mimicCert.resetAndLock(mimicCert);

        ACLFilledChecklist checklist(NULL, sslServerBump->request.getRaw(),
                                     clientConnection != NULL ? clientConnection->rfc931 : dash_str);
        checklist.sslErrors = cbdataReference(sslServerBump->sslErrors);

        for (sslproxy_cert_adapt *ca = Config.ssl_client.cert_adapt; ca != NULL; ca = ca->next) {
            // If the algorithm already set, then ignore it.
            if ((ca->alg == Ssl::algSetCommonName && certProperties.setCommonName) ||
                    (ca->alg == Ssl::algSetValidAfter && certProperties.setValidAfter) ||
                    (ca->alg == Ssl::algSetValidBefore && certProperties.setValidBefore) )
                continue;

            if (ca->aclList && checklist.fastCheck(ca->aclList) == ACCESS_ALLOWED) {
                const char *alg = Ssl::CertAdaptAlgorithmStr[ca->alg];
                const char *param = ca->param;

                // For parameterless CN adaptation, use hostname from the
                // CONNECT request.
                if (ca->alg == Ssl::algSetCommonName) {
                    if (!param)
                        param = sslConnectHostOrIp.termedBuf();
                    certProperties.commonName = param;
                    certProperties.setCommonName = true;
                } else if (ca->alg == Ssl::algSetValidAfter)
                    certProperties.setValidAfter = true;
                else if (ca->alg == Ssl::algSetValidBefore)
                    certProperties.setValidBefore = true;

                debugs(33, 5, HERE << "Matches certificate adaptation aglorithm: " <<
                       alg << " param: " << (param ? param : "-"));
            }
        }

        certProperties.signAlgorithm = Ssl::algSignEnd;
        for (sslproxy_cert_sign *sg = Config.ssl_client.cert_sign; sg != NULL; sg = sg->next) {
            if (sg->aclList && checklist.fastCheck(sg->aclList) == ACCESS_ALLOWED) {
                certProperties.signAlgorithm = (Ssl::CertSignAlgorithm)sg->alg;
                break;
            }
        }
    } else {// if (!sslServerBump->entry->isEmpty())
        // Use trusted certificate for a Squid-generated error
        // or the user would have to add a security exception
        // just to see the error page. We will close the connection
        // so that the trust is not extended to non-Squid content.
        certProperties.signAlgorithm = Ssl::algSignTrusted;
    }

    assert(certProperties.signAlgorithm != Ssl::algSignEnd);

    if (certProperties.signAlgorithm == Ssl::algSignUntrusted) {
        assert(port->untrustedSigningCert.get());
        certProperties.signWithX509.resetAndLock(port->untrustedSigningCert.get());
        certProperties.signWithPkey.resetAndLock(port->untrustedSignPkey.get());
    } else {
        assert(port->signingCert.get());
        certProperties.signWithX509.resetAndLock(port->signingCert.get());

        if (port->signPkey.get())
            certProperties.signWithPkey.resetAndLock(port->signPkey.get());
    }
    signAlgorithm = certProperties.signAlgorithm;

    certProperties.signHash = Ssl::DefaultSignHash;
}

void
ConnStateData::getSslContextStart()
{
    assert(areAllContextsForThisConnection());
    freeAllContexts();
    /* careful: freeAllContexts() above frees request, host, etc. */

    if (port->generateHostCertificates) {
        Ssl::CertificateProperties certProperties;
        buildSslCertGenerationParams(certProperties);
        sslBumpCertKey = certProperties.dbKey().c_str();
        assert(sslBumpCertKey.size() > 0 && sslBumpCertKey[0] != '\0');

        // Disable caching for bumpPeekAndSplice mode
        if (!(sslServerBump && (sslServerBump->act.step1 == Ssl::bumpPeek || sslServerBump->act.step1 == Ssl::bumpStare))) {
            debugs(33, 5, "Finding SSL certificate for " << sslBumpCertKey << " in cache");
            Ssl::LocalContextStorage * ssl_ctx_cache = Ssl::TheGlobalContextStorage.getLocalStorage(port->s);
            SSL_CTX * dynCtx = NULL;
            Ssl::SSL_CTX_Pointer *cachedCtx = ssl_ctx_cache ? ssl_ctx_cache->get(sslBumpCertKey.termedBuf()) : NULL;
            if (cachedCtx && (dynCtx = cachedCtx->get())) {
                debugs(33, 5, "SSL certificate for " << sslBumpCertKey << " found in cache");
                if (Ssl::verifySslCertificate(dynCtx, certProperties)) {
                    debugs(33, 5, "Cached SSL certificate for " << sslBumpCertKey << " is valid");
                    getSslContextDone(dynCtx);
                    return;
                } else {
                    debugs(33, 5, "Cached SSL certificate for " << sslBumpCertKey << " is out of date. Delete this certificate from cache");
                    if (ssl_ctx_cache)
                        ssl_ctx_cache->del(sslBumpCertKey.termedBuf());
                }
            } else {
                debugs(33, 5, "SSL certificate for " << sslBumpCertKey << " haven't found in cache");
            }
        }

#if USE_SSL_CRTD
        try {
            debugs(33, 5, HERE << "Generating SSL certificate for " << certProperties.commonName << " using ssl_crtd.");
            Ssl::CrtdMessage request_message(Ssl::CrtdMessage::REQUEST);
            request_message.setCode(Ssl::CrtdMessage::code_new_certificate);
            request_message.composeRequest(certProperties);
            debugs(33, 5, HERE << "SSL crtd request: " << request_message.compose().c_str());
            Ssl::Helper::GetInstance()->sslSubmit(request_message, sslCrtdHandleReplyWrapper, this);
            return;
        } catch (const std::exception &e) {
            debugs(33, DBG_IMPORTANT, "ERROR: Failed to compose ssl_crtd " <<
                   "request for " << certProperties.commonName <<
                   " certificate: " << e.what() << "; will now block to " <<
                   "generate that certificate.");
            // fall through to do blocking in-process generation.
        }
#endif // USE_SSL_CRTD

        debugs(33, 5, HERE << "Generating SSL certificate for " << certProperties.commonName);
        if (sslServerBump && (sslServerBump->act.step1 == Ssl::bumpPeek || sslServerBump->act.step1 == Ssl::bumpStare)) {
            doPeekAndSpliceStep();
            SSL *ssl = fd_table[clientConnection->fd].ssl;
            if (!Ssl::configureSSL(ssl, certProperties, *port))
                debugs(33, 5, "Failed to set certificates to ssl object for PeekAndSplice mode");
        } else {
            SSL_CTX *dynCtx = Ssl::generateSslContext(certProperties, *port);
            getSslContextDone(dynCtx, true);
        }
        return;
    }
    getSslContextDone(NULL);
}

void
ConnStateData::getSslContextDone(SSL_CTX * sslContext, bool isNew)
{
    // Try to add generated ssl context to storage.
    if (port->generateHostCertificates && isNew) {

        if (signAlgorithm == Ssl::algSignTrusted) {
            // Add signing certificate to the certificates chain
            X509 *cert = port->signingCert.get();
            if (SSL_CTX_add_extra_chain_cert(sslContext, cert)) {
                // increase the certificate lock
                CRYPTO_add(&(cert->references),1,CRYPTO_LOCK_X509);
            } else {
                const int ssl_error = ERR_get_error();
                debugs(33, DBG_IMPORTANT, "WARNING: can not add signing certificate to SSL context chain: " << ERR_error_string(ssl_error, NULL));
            }
            Ssl::addChainToSslContext(sslContext, port->certsToChain.get());
        }
        //else it is self-signed or untrusted do not attrach any certificate

        Ssl::LocalContextStorage *ssl_ctx_cache = Ssl::TheGlobalContextStorage.getLocalStorage(port->s);
        assert(sslBumpCertKey.size() > 0 && sslBumpCertKey[0] != '\0');
        if (sslContext) {
            if (!ssl_ctx_cache || !ssl_ctx_cache->add(sslBumpCertKey.termedBuf(), new Ssl::SSL_CTX_Pointer(sslContext))) {
                // If it is not in storage delete after using. Else storage deleted it.
                fd_table[clientConnection->fd].dynamicSslContext = sslContext;
            }
        } else {
            debugs(33, 2, HERE << "Failed to generate SSL cert for " << sslConnectHostOrIp);
        }
    }

    // If generated ssl context = NULL, try to use static ssl context.
    if (!sslContext) {
        if (!port->staticSslContext) {
            debugs(83, DBG_IMPORTANT, "Closing SSL " << clientConnection->remote << " as lacking SSL context");
            clientConnection->close();
            return;
        } else {
            debugs(33, 5, HERE << "Using static ssl context.");
            sslContext = port->staticSslContext.get();
        }
    }

    if (!httpsCreate(clientConnection, sslContext))
        return;

    // bumped intercepted conns should already have Config.Timeout.request set
    // but forwarded connections may only have Config.Timeout.lifetime. [Re]set
    // to make sure the connection does not get stuck on non-SSL clients.
    typedef CommCbMemFunT<ConnStateData, CommTimeoutCbParams> TimeoutDialer;
    AsyncCall::Pointer timeoutCall = JobCallback(33, 5, TimeoutDialer,
                                     this, ConnStateData::requestTimeout);
    commSetConnTimeout(clientConnection, Config.Timeout.request, timeoutCall);

    // Disable the client read handler until CachePeer selection is complete
    Comm::SetSelect(clientConnection->fd, COMM_SELECT_READ, NULL, NULL, 0);
    Comm::SetSelect(clientConnection->fd, COMM_SELECT_READ, clientNegotiateSSL, this, 0);
    switchedToHttps_ = true;
}

void
ConnStateData::switchToHttps(HttpRequest *request, Ssl::BumpMode bumpServerMode)
{
    assert(!switchedToHttps_);

    sslConnectHostOrIp = request->GetHost();
    sslCommonName = request->GetHost();

    // We are going to read new request
    flags.readMore = true;
    debugs(33, 5, HERE << "converting " << clientConnection << " to SSL");

    // keep version major.minor details the same.
    // but we are now performing the HTTPS handshake traffic
    transferProtocol.protocol = AnyP::PROTO_HTTPS;

    // If sslServerBump is set, then we have decided to deny CONNECT
    // and now want to switch to SSL to send the error to the client
    // without even peeking at the origin server certificate.
    if (bumpServerMode == Ssl::bumpServerFirst && !sslServerBump) {
        request->flags.sslPeek = true;
        sslServerBump = new Ssl::ServerBump(request);

        // will call httpsPeeked() with certificate and connection, eventually
        FwdState::fwdStart(clientConnection, sslServerBump->entry, sslServerBump->request.getRaw());
        return;
    } else if (bumpServerMode == Ssl::bumpPeek || bumpServerMode == Ssl::bumpStare) {
        request->flags.sslPeek = true;
        sslServerBump = new Ssl::ServerBump(request, NULL, bumpServerMode);
        startPeekAndSplice();
        return;
    }

    // otherwise, use sslConnectHostOrIp
    getSslContextStart();
}

bool
ConnStateData::spliceOnError(const err_type err)
{
    if (Config.accessList.on_unsupported_protocol) {
        assert(serverBump());
        ACLFilledChecklist checklist(Config.accessList.on_unsupported_protocol, serverBump()->request.getRaw(), NULL);
        checklist.requestErrorType = err;
        checklist.conn(this);
        allow_t answer = checklist.fastCheck();
        if (answer == ACCESS_ALLOWED && answer.kind == 1) {
            splice();
            return true;
        }
    }
    return false;
}

/** negotiate an SSL connection */
static void
clientPeekAndSpliceSSL(int fd, void *data)
{
    ConnStateData *conn = (ConnStateData *)data;
    SSL *ssl = fd_table[fd].ssl;

    debugs(83, 5, "Start peek and splice on FD " << fd);

    int ret = 0;
    if ((ret = Squid_SSL_accept(conn, clientPeekAndSpliceSSL)) < 0)
        debugs(83, 2, "SSL_accept failed.");

    BIO *b = SSL_get_rbio(ssl);
    assert(b);
    Ssl::ClientBio *bio = static_cast<Ssl::ClientBio *>(b->ptr);
    if (ret < 0) {
        const err_type err = bio->noSslClient() ? ERR_PROTOCOL_UNKNOWN : ERR_SECURE_ACCEPT_FAIL;
        if (!conn->spliceOnError(err))
            conn->clientConnection->close();
        return;
    }

    if (bio->rBufData().contentSize() > 0)
        conn->receivedFirstByte();

    if (bio->gotHello()) {
        if (conn->serverBump()) {
            Ssl::Bio::sslFeatures const &features = bio->getFeatures();
            if (!features.serverName.isEmpty())
                conn->serverBump()->clientSni = features.serverName;
        }

        debugs(83, 5, "I got hello. Start forwarding the request!!! ");
        Comm::SetSelect(fd, COMM_SELECT_READ, NULL, NULL, 0);
        Comm::SetSelect(fd, COMM_SELECT_WRITE, NULL, NULL, 0);
        conn->startPeekAndSpliceDone();
        return;
    }
}

void ConnStateData::startPeekAndSplice()
{
    // will call httpsPeeked() with certificate and connection, eventually
    SSL_CTX *unConfiguredCTX = Ssl::createSSLContext(port->signingCert, port->signPkey, *port);
    fd_table[clientConnection->fd].dynamicSslContext = unConfiguredCTX;

    if (!httpsCreate(clientConnection, unConfiguredCTX))
        return;

    // commSetConnTimeout() was called for this request before we switched.
    // Fix timeout to request_start_timeout
    typedef CommCbMemFunT<ConnStateData, CommTimeoutCbParams> TimeoutDialer;
    AsyncCall::Pointer timeoutCall =  JobCallback(33, 5,
                                      TimeoutDialer, this, ConnStateData::requestTimeout);
    commSetConnTimeout(clientConnection, Config.Timeout.request_start_timeout, timeoutCall);
    // Also reset receivedFirstByte_ flag to allow this timeout work in the case we have
    // a bumbed "connect" request on non transparent port.
    receivedFirstByte_ = false;

    // Disable the client read handler until CachePeer selection is complete
    Comm::SetSelect(clientConnection->fd, COMM_SELECT_READ, NULL, NULL, 0);
    Comm::SetSelect(clientConnection->fd, COMM_SELECT_READ, clientPeekAndSpliceSSL, this, 0);
    switchedToHttps_ = true;

    SSL *ssl = fd_table[clientConnection->fd].ssl;
    BIO *b = SSL_get_rbio(ssl);
    Ssl::ClientBio *bio = static_cast<Ssl::ClientBio *>(b->ptr);
    bio->hold(true);
}

void httpsSslBumpStep2AccessCheckDone(allow_t answer, void *data)
{
    ConnStateData *connState = (ConnStateData *) data;

    // if the connection is closed or closing, just return.
    if (!connState->isOpen())
        return;

    debugs(33, 5, "Answer: " << answer << " kind:" << answer.kind);
    assert(connState->serverBump());
    Ssl::BumpMode bumpAction;
    if (answer == ACCESS_ALLOWED) {
        if (answer.kind == Ssl::bumpNone)
            bumpAction = Ssl::bumpSplice;
        else if (answer.kind == Ssl::bumpClientFirst || answer.kind == Ssl::bumpServerFirst)
            bumpAction = Ssl::bumpBump;
        else
            bumpAction = (Ssl::BumpMode)answer.kind;
    } else
        bumpAction = Ssl::bumpSplice;

    connState->serverBump()->act.step2 = bumpAction;
    connState->sslBumpMode = bumpAction;

    if (bumpAction == Ssl::bumpTerminate) {
        comm_close(connState->clientConnection->fd);
    } else if (bumpAction != Ssl::bumpSplice) {
        connState->startPeekAndSpliceDone();
    } else
        connState->splice();
}

void
ConnStateData::splice()
{
    //Normally we can splice here, because we just got client hello message
    SSL *ssl = fd_table[clientConnection->fd].ssl;
    BIO *b = SSL_get_rbio(ssl);
    Ssl::ClientBio *bio = static_cast<Ssl::ClientBio *>(b->ptr);
    MemBuf const &rbuf = bio->rBufData();
    debugs(83,5, "Bio for  " << clientConnection << " read " << rbuf.contentSize() << " helo bytes");
    // Do splice:
    fd_table[clientConnection->fd].read_method = &default_read_method;
    fd_table[clientConnection->fd].write_method = &default_write_method;

    if (transparent()) {
        // set the current protocol to something sensible (was "HTTPS" for the bumping process)
        // we are sending a faked-up HTTP/1.1 message wrapper, so go with that.
        transferProtocol = Http::ProtocolVersion();
        // fake a CONNECT request to force connState to tunnel
        static char ip[MAX_IPSTRLEN];
        clientConnection->local.toUrl(ip, sizeof(ip));
        in.buf.assign("CONNECT ").append(ip).append(" HTTP/1.1\r\nHost: ").append(ip).append("\r\n\r\n").append(rbuf.content(), rbuf.contentSize());
        bool ret = handleReadData();
        if (ret)
            ret = clientParseRequests();

        if (!ret) {
            debugs(33, 2, "Failed to start fake CONNECT request for ssl spliced connection: " << clientConnection);
            clientConnection->close();
        }
    } else {
        // XXX: assuming that there was an HTTP/1.1 CONNECT to begin with...

        // reset the current protocol to HTTP/1.1 (was "HTTPS" for the bumping process)
        transferProtocol = Http::ProtocolVersion();
        // in.buf still has the "CONNECT ..." request data, reset it to SSL hello message
        in.buf.append(rbuf.content(), rbuf.contentSize());
        ClientSocketContext::Pointer context = getCurrentContext();
        ClientHttpRequest *http = context->http;
        tunnelStart(http, &http->out.size, &http->al->http.code, http->al);
    }
}

void
ConnStateData::startPeekAndSpliceDone()
{
    // This is the Step2 of the SSL bumping
    assert(sslServerBump);
    if (sslServerBump->step == Ssl::bumpStep1) {
        sslServerBump->step = Ssl::bumpStep2;
        // Run a accessList check to check if want to splice or continue bumping

        ACLFilledChecklist *acl_checklist = new ACLFilledChecklist(Config.accessList.ssl_bump, sslServerBump->request.getRaw(), NULL);
        //acl_checklist->src_addr = params.conn->remote;
        //acl_checklist->my_addr = s->s;
        acl_checklist->nonBlockingCheck(httpsSslBumpStep2AccessCheckDone, this);
        return;
    }

    FwdState::fwdStart(clientConnection, sslServerBump->entry, sslServerBump->request.getRaw());
}

void
ConnStateData::doPeekAndSpliceStep()
{
    SSL *ssl = fd_table[clientConnection->fd].ssl;
    BIO *b = SSL_get_rbio(ssl);
    assert(b);
    Ssl::ClientBio *bio = static_cast<Ssl::ClientBio *>(b->ptr);

    debugs(33, 5, "PeekAndSplice mode, proceed with client negotiation. Currrent state:" << SSL_state_string_long(ssl));
    bio->hold(false);

    Comm::SetSelect(clientConnection->fd, COMM_SELECT_WRITE, clientNegotiateSSL, this, 0);
    switchedToHttps_ = true;
}

void
ConnStateData::httpsPeeked(Comm::ConnectionPointer serverConnection)
{
    Must(sslServerBump != NULL);

    if (Comm::IsConnOpen(serverConnection)) {
        SSL *ssl = fd_table[serverConnection->fd].ssl;
        assert(ssl);
        Ssl::X509_Pointer serverCert(SSL_get_peer_certificate(ssl));
        assert(serverCert.get() != NULL);
        sslCommonName = Ssl::CommonHostName(serverCert.get());
        debugs(33, 5, HERE << "HTTPS server CN: " << sslCommonName <<
               " bumped: " << *serverConnection);

        pinConnection(serverConnection, NULL, NULL, false);

        debugs(33, 5, HERE << "bumped HTTPS server: " << sslConnectHostOrIp);
    } else {
        debugs(33, 5, HERE << "Error while bumping: " << sslConnectHostOrIp);
        Ip::Address intendedDest;
        intendedDest = sslConnectHostOrIp.termedBuf();
        const bool isConnectRequest = !port->flags.isIntercepted();

        // Squid serves its own error page and closes, so we want
        // a CN that causes no additional browser errors. Possible
        // only when bumping CONNECT with a user-typed address.
        if (intendedDest.isAnyAddr() || isConnectRequest)
            sslCommonName = sslConnectHostOrIp;
        else if (sslServerBump->serverCert.get())
            sslCommonName = Ssl::CommonHostName(sslServerBump->serverCert.get());

        //  copy error detail from bump-server-first request to CONNECT request
        if (currentobject != NULL && currentobject->http != NULL && currentobject->http->request)
            currentobject->http->request->detailError(sslServerBump->request->errType, sslServerBump->request->errDetail);
    }

    getSslContextStart();
}

#endif /* USE_OPENSSL */

/// check FD after clientHttp[s]ConnectionOpened, adjust HttpSockets as needed
static bool
OpenedHttpSocket(const Comm::ConnectionPointer &c, const Ipc::FdNoteId portType)
{
    if (!Comm::IsConnOpen(c)) {
        Must(NHttpSockets > 0); // we tried to open some
        --NHttpSockets; // there will be fewer sockets than planned
        Must(HttpSockets[NHttpSockets] < 0); // no extra fds received

        if (!NHttpSockets) // we could not open any listen sockets at all
            fatalf("Unable to open %s",FdNote(portType));

        return false;
    }
    return true;
}

/// find any unused HttpSockets[] slot and store fd there or return false
static bool
AddOpenedHttpSocket(const Comm::ConnectionPointer &conn)
{
    bool found = false;
    for (int i = 0; i < NHttpSockets && !found; ++i) {
        if ((found = HttpSockets[i] < 0))
            HttpSockets[i] = conn->fd;
    }
    return found;
}

static void
clientHttpConnectionsOpen(void)
{
    for (AnyP::PortCfgPointer s = HttpPortList; s != NULL; s = s->next) {
        if (MAXTCPLISTENPORTS == NHttpSockets) {
            debugs(1, DBG_IMPORTANT, "WARNING: You have too many 'http_port' lines.");
            debugs(1, DBG_IMPORTANT, "         The limit is " << MAXTCPLISTENPORTS << " HTTP ports.");
            continue;
        }

#if USE_OPENSSL
        if (s->flags.tunnelSslBumping && !Config.accessList.ssl_bump) {
            debugs(33, DBG_IMPORTANT, "WARNING: No ssl_bump configured. Disabling ssl-bump on " << AnyP::UriScheme(s->transport.protocol) << "_port " << s->s);
            s->flags.tunnelSslBumping = false;
        }

        if (s->flags.tunnelSslBumping &&
                !s->staticSslContext &&
                !s->generateHostCertificates) {
            debugs(1, DBG_IMPORTANT, "Will not bump SSL at http_port " << s->s << " due to SSL initialization failure.");
            s->flags.tunnelSslBumping = false;
        }
        if (s->flags.tunnelSslBumping) {
            // Create ssl_ctx cache for this port.
            Ssl::TheGlobalContextStorage.addLocalStorage(s->s, s->dynamicCertMemCacheSize == std::numeric_limits<size_t>::max() ? 4194304 : s->dynamicCertMemCacheSize);
        }
#endif

        // Fill out a Comm::Connection which IPC will open as a listener for us
        //  then pass back when active so we can start a TcpAcceptor subscription.
        s->listenConn = new Comm::Connection;
        s->listenConn->local = s->s;
        s->listenConn->flags = COMM_NONBLOCKING | (s->flags.tproxyIntercept ? COMM_TRANSPARENT : 0) | (s->flags.natIntercept ? COMM_INTERCEPTION : 0);

        // setup the subscriptions such that new connections accepted by listenConn are handled by HTTP
        typedef CommCbFunPtrCallT<CommAcceptCbPtrFun> AcceptCall;
        RefCount<AcceptCall> subCall = commCbCall(5, 5, "httpAccept", CommAcceptCbPtrFun(httpAccept, CommAcceptCbParams(NULL)));
        Subscription::Pointer sub = new CallSubscription<AcceptCall>(subCall);

        AsyncCall::Pointer listenCall = asyncCall(33,2, "clientListenerConnectionOpened",
                                        ListeningStartedDialer(&clientListenerConnectionOpened, s, Ipc::fdnHttpSocket, sub));
        Ipc::StartListening(SOCK_STREAM, IPPROTO_TCP, s->listenConn, Ipc::fdnHttpSocket, listenCall);

        HttpSockets[NHttpSockets] = -1; // set in clientListenerConnectionOpened
        ++NHttpSockets;
    }
}

#if USE_OPENSSL
static void
clientHttpsConnectionsOpen(void)
{
    for (AnyP::PortCfgPointer s = HttpsPortList; s != NULL; s = s->next) {
        if (MAXTCPLISTENPORTS == NHttpSockets) {
            debugs(1, DBG_IMPORTANT, "Ignoring 'https_port' lines exceeding the limit.");
            debugs(1, DBG_IMPORTANT, "The limit is " << MAXTCPLISTENPORTS << " HTTPS ports.");
            continue;
        }

        if (!s->staticSslContext) {
            debugs(1, DBG_IMPORTANT, "Ignoring https_port " << s->s <<
                   " due to SSL initialization failure.");
            continue;
        }

        // TODO: merge with similar code in clientHttpConnectionsOpen()
        if (s->flags.tunnelSslBumping && !Config.accessList.ssl_bump) {
            debugs(33, DBG_IMPORTANT, "WARNING: No ssl_bump configured. Disabling ssl-bump on " << AnyP::UriScheme(s->transport.protocol) << "_port " << s->s);
            s->flags.tunnelSslBumping = false;
        }

        if (s->flags.tunnelSslBumping && !s->staticSslContext && !s->generateHostCertificates) {
            debugs(1, DBG_IMPORTANT, "Will not bump SSL at http_port " << s->s << " due to SSL initialization failure.");
            s->flags.tunnelSslBumping = false;
        }

        if (s->flags.tunnelSslBumping) {
            // Create ssl_ctx cache for this port.
            Ssl::TheGlobalContextStorage.addLocalStorage(s->s, s->dynamicCertMemCacheSize == std::numeric_limits<size_t>::max() ? 4194304 : s->dynamicCertMemCacheSize);
        }

        // Fill out a Comm::Connection which IPC will open as a listener for us
        s->listenConn = new Comm::Connection;
        s->listenConn->local = s->s;
        s->listenConn->flags = COMM_NONBLOCKING | (s->flags.tproxyIntercept ? COMM_TRANSPARENT : 0) |
                               (s->flags.natIntercept ? COMM_INTERCEPTION : 0);

        // setup the subscriptions such that new connections accepted by listenConn are handled by HTTPS
        typedef CommCbFunPtrCallT<CommAcceptCbPtrFun> AcceptCall;
        RefCount<AcceptCall> subCall = commCbCall(5, 5, "httpsAccept", CommAcceptCbPtrFun(httpsAccept, CommAcceptCbParams(NULL)));
        Subscription::Pointer sub = new CallSubscription<AcceptCall>(subCall);

        AsyncCall::Pointer listenCall = asyncCall(33, 2, "clientListenerConnectionOpened",
                                        ListeningStartedDialer(&clientListenerConnectionOpened,
                                                s, Ipc::fdnHttpsSocket, sub));
        Ipc::StartListening(SOCK_STREAM, IPPROTO_TCP, s->listenConn, Ipc::fdnHttpsSocket, listenCall);
        HttpSockets[NHttpSockets] = -1;
        ++NHttpSockets;
    }
}
#endif

void
clientStartListeningOn(AnyP::PortCfgPointer &port, const RefCount< CommCbFunPtrCallT<CommAcceptCbPtrFun> > &subCall, const Ipc::FdNoteId fdNote)
{
    // Fill out a Comm::Connection which IPC will open as a listener for us
    port->listenConn = new Comm::Connection;
    port->listenConn->local = port->s;
    port->listenConn->flags =
        COMM_NONBLOCKING |
        (port->flags.tproxyIntercept ? COMM_TRANSPARENT : 0) |
        (port->flags.natIntercept ? COMM_INTERCEPTION : 0);

    // route new connections to subCall
    typedef CommCbFunPtrCallT<CommAcceptCbPtrFun> AcceptCall;
    Subscription::Pointer sub = new CallSubscription<AcceptCall>(subCall);
    AsyncCall::Pointer listenCall =
        asyncCall(33, 2, "clientListenerConnectionOpened",
                  ListeningStartedDialer(&clientListenerConnectionOpened,
                                         port, fdNote, sub));
    Ipc::StartListening(SOCK_STREAM, IPPROTO_TCP, port->listenConn, fdNote, listenCall);

    assert(NHttpSockets < MAXTCPLISTENPORTS);
    HttpSockets[NHttpSockets] = -1;
    ++NHttpSockets;
}

/// process clientHttpConnectionsOpen result
static void
clientListenerConnectionOpened(AnyP::PortCfgPointer &s, const Ipc::FdNoteId portTypeNote, const Subscription::Pointer &sub)
{
    Must(s != NULL);

    if (!OpenedHttpSocket(s->listenConn, portTypeNote))
        return;

    Must(Comm::IsConnOpen(s->listenConn));

    // TCP: setup a job to handle accept() with subscribed handler
    AsyncJob::Start(new Comm::TcpAcceptor(s, FdNote(portTypeNote), sub));

    debugs(1, DBG_IMPORTANT, "Accepting " <<
           (s->flags.natIntercept ? "NAT intercepted " : "") <<
           (s->flags.tproxyIntercept ? "TPROXY intercepted " : "") <<
           (s->flags.tunnelSslBumping ? "SSL bumped " : "") <<
           (s->flags.accelSurrogate ? "reverse-proxy " : "")
           << FdNote(portTypeNote) << " connections at "
           << s->listenConn);

    Must(AddOpenedHttpSocket(s->listenConn)); // otherwise, we have received a fd we did not ask for
}

void
clientOpenListenSockets(void)
{
    clientHttpConnectionsOpen();
#if USE_OPENSSL
    clientHttpsConnectionsOpen();
#endif
    Ftp::StartListening();

    if (NHttpSockets < 1)
        fatal("No HTTP, HTTPS, or FTP ports configured");
}

void
clientConnectionsClose()
{
    for (AnyP::PortCfgPointer s = HttpPortList; s != NULL; s = s->next) {
        if (s->listenConn != NULL) {
            debugs(1, DBG_IMPORTANT, "Closing HTTP port " << s->listenConn->local);
            s->listenConn->close();
            s->listenConn = NULL;
        }
    }

#if USE_OPENSSL
    for (AnyP::PortCfgPointer s = HttpsPortList; s != NULL; s = s->next) {
        if (s->listenConn != NULL) {
            debugs(1, DBG_IMPORTANT, "Closing HTTPS port " << s->listenConn->local);
            s->listenConn->close();
            s->listenConn = NULL;
        }
    }
#endif

    Ftp::StopListening();

    // TODO see if we can drop HttpSockets array entirely */
    for (int i = 0; i < NHttpSockets; ++i) {
        HttpSockets[i] = -1;
    }

    NHttpSockets = 0;
}

int
varyEvaluateMatch(StoreEntry * entry, HttpRequest * request)
{
    const char *vary = request->vary_headers;
    int has_vary = entry->getReply()->header.has(HDR_VARY);
#if X_ACCELERATOR_VARY

    has_vary |=
        entry->getReply()->header.has(HDR_X_ACCELERATOR_VARY);
#endif

    if (!has_vary || !entry->mem_obj->vary_headers) {
        if (vary) {
            /* Oops... something odd is going on here.. */
            debugs(33, DBG_IMPORTANT, "varyEvaluateMatch: Oops. Not a Vary object on second attempt, '" <<
                   entry->mem_obj->urlXXX() << "' '" << vary << "'");
            safe_free(request->vary_headers);
            return VARY_CANCEL;
        }

        if (!has_vary) {
            /* This is not a varying object */
            return VARY_NONE;
        }

        /* virtual "vary" object found. Calculate the vary key and
         * continue the search
         */
        vary = httpMakeVaryMark(request, entry->getReply());

        if (vary) {
            request->vary_headers = xstrdup(vary);
            return VARY_OTHER;
        } else {
            /* Ouch.. we cannot handle this kind of variance */
            /* XXX This cannot really happen, but just to be complete */
            return VARY_CANCEL;
        }
    } else {
        if (!vary) {
            vary = httpMakeVaryMark(request, entry->getReply());

            if (vary)
                request->vary_headers = xstrdup(vary);
        }

        if (!vary) {
            /* Ouch.. we cannot handle this kind of variance */
            /* XXX This cannot really happen, but just to be complete */
            return VARY_CANCEL;
        } else if (strcmp(vary, entry->mem_obj->vary_headers) == 0) {
            return VARY_MATCH;
        } else {
            /* Oops.. we have already been here and still haven't
             * found the requested variant. Bail out
             */
            debugs(33, DBG_IMPORTANT, "varyEvaluateMatch: Oops. Not a Vary match on second attempt, '" <<
                   entry->mem_obj->urlXXX() << "' '" << vary << "'");
            return VARY_CANCEL;
        }
    }
}

ACLFilledChecklist *
clientAclChecklistCreate(const acl_access * acl, ClientHttpRequest * http)
{
    ConnStateData * conn = http->getConn();
    ACLFilledChecklist *ch = new ACLFilledChecklist(acl, http->request,
            cbdataReferenceValid(conn) && conn != NULL && conn->clientConnection != NULL ? conn->clientConnection->rfc931 : dash_str);
    ch->al = http->al;
    /*
     * hack for ident ACL. It needs to get full addresses, and a place to store
     * the ident result on persistent connections...
     */
    /* connection oriented auth also needs these two lines for it's operation. */
    return ch;
}

bool
ConnStateData::transparent() const
{
    return clientConnection != NULL && (clientConnection->flags & (COMM_TRANSPARENT|COMM_INTERCEPTION));
}

bool
ConnStateData::reading() const
{
    return reader != NULL;
}

void
ConnStateData::stopReading()
{
    if (reading()) {
        Comm::ReadCancel(clientConnection->fd, reader);
        reader = NULL;
    }
}

BodyPipe::Pointer
ConnStateData::expectRequestBody(int64_t size)
{
    bodyPipe = new BodyPipe(this);
    if (size >= 0)
        bodyPipe->setBodySize(size);
    else
        startDechunkingRequest();
    return bodyPipe;
}

int64_t
ConnStateData::mayNeedToReadMoreBody() const
{
    if (!bodyPipe)
        return 0; // request without a body or read/produced all body bytes

    if (!bodyPipe->bodySizeKnown())
        return -1; // probably need to read more, but we cannot be sure

    const int64_t needToProduce = bodyPipe->unproducedSize();
    const int64_t haveAvailable = static_cast<int64_t>(in.buf.length());

    if (needToProduce <= haveAvailable)
        return 0; // we have read what we need (but are waiting for pipe space)

    return needToProduce - haveAvailable;
}

void
ConnStateData::stopReceiving(const char *error)
{
    debugs(33, 4, HERE << "receiving error (" << clientConnection << "): " << error <<
           "; old sending error: " <<
           (stoppedSending() ? stoppedSending_ : "none"));

    if (const char *oldError = stoppedReceiving()) {
        debugs(33, 3, HERE << "already stopped receiving: " << oldError);
        return; // nothing has changed as far as this connection is concerned
    }

    stoppedReceiving_ = error;

    if (const char *sendError = stoppedSending()) {
        debugs(33, 3, HERE << "closing because also stopped sending: " << sendError);
        clientConnection->close();
    }
}

void
ConnStateData::expectNoForwarding()
{
    if (bodyPipe != NULL) {
        debugs(33, 4, HERE << "no consumer for virgin body " << bodyPipe->status());
        bodyPipe->expectNoConsumption();
    }
}

/// initialize dechunking state
void
ConnStateData::startDechunkingRequest()
{
    Must(bodyPipe != NULL);
    debugs(33, 5, HERE << "start dechunking" << bodyPipe->status());
    assert(!in.bodyParser);
    in.bodyParser = new ChunkedCodingParser;
}

/// put parsed content into input buffer and clean up
void
ConnStateData::finishDechunkingRequest(bool withSuccess)
{
    debugs(33, 5, HERE << "finish dechunking: " << withSuccess);

    if (bodyPipe != NULL) {
        debugs(33, 7, HERE << "dechunked tail: " << bodyPipe->status());
        BodyPipe::Pointer myPipe = bodyPipe;
        stopProducingFor(bodyPipe, withSuccess); // sets bodyPipe->bodySize()
        Must(!bodyPipe); // we rely on it being nil after we are done with body
        if (withSuccess) {
            Must(myPipe->bodySizeKnown());
            ClientSocketContext::Pointer context = getCurrentContext();
            if (context != NULL && context->http && context->http->request)
                context->http->request->setContentLength(myPipe->bodySize());
        }
    }

    delete in.bodyParser;
    in.bodyParser = NULL;
}

ConnStateData::In::In() :
    bodyParser(NULL),
    buf()
{}

ConnStateData::In::~In()
{
    delete bodyParser; // TODO: pool
}

void
ConnStateData::sendControlMsg(HttpControlMsg msg)
{
    if (!isOpen()) {
        debugs(33, 3, HERE << "ignoring 1xx due to earlier closure");
        return;
    }

    ClientSocketContext::Pointer context = getCurrentContext();
    if (context != NULL) {
        context->writeControlMsg(msg); // will call msg.cbSuccess
        return;
    }

    debugs(33, 3, HERE << " closing due to missing context for 1xx");
    clientConnection->close();
}

/// Our close handler called by Comm when the pinned connection is closed
void
ConnStateData::clientPinnedConnectionClosed(const CommCloseCbParams &io)
{
    // FwdState might repin a failed connection sooner than this close
    // callback is called for the failed connection.
    assert(pinning.serverConnection == io.conn);
    pinning.closeHandler = NULL; // Comm unregisters handlers before calling
    const bool sawZeroReply = pinning.zeroReply; // reset when unpinning
    unpinConnection(false);

    if (sawZeroReply && clientConnection != NULL) {
        debugs(33, 3, "Closing client connection on pinned zero reply.");
        clientConnection->close();
    }

}

void
ConnStateData::pinConnection(const Comm::ConnectionPointer &pinServer, HttpRequest *request, CachePeer *aPeer, bool auth, bool monitor)
{
    if (!Comm::IsConnOpen(pinning.serverConnection) ||
            pinning.serverConnection->fd != pinServer->fd)
        pinNewConnection(pinServer, request, aPeer, auth);

    if (monitor)
        startPinnedConnectionMonitoring();
}

void
ConnStateData::pinNewConnection(const Comm::ConnectionPointer &pinServer, HttpRequest *request, CachePeer *aPeer, bool auth)
{
    unpinConnection(true); // closes pinned connection, if any, and resets fields

    pinning.serverConnection = pinServer;

    debugs(33, 3, HERE << pinning.serverConnection);

    Must(pinning.serverConnection != NULL);

    // when pinning an SSL bumped connection, the request may be NULL
    const char *pinnedHost = "[unknown]";
    if (request) {
        pinning.host = xstrdup(request->GetHost());
        pinning.port = request->port;
        pinnedHost = pinning.host;
    } else {
        pinning.port = pinServer->remote.port();
    }
    pinning.pinned = true;
    if (aPeer)
        pinning.peer = cbdataReference(aPeer);
    pinning.auth = auth;
    char stmp[MAX_IPSTRLEN];
    char desc[FD_DESC_SZ];
    snprintf(desc, FD_DESC_SZ, "%s pinned connection for %s (%d)",
             (auth || !aPeer) ? pinnedHost : aPeer->name,
             clientConnection->remote.toUrl(stmp,MAX_IPSTRLEN),
             clientConnection->fd);
    fd_note(pinning.serverConnection->fd, desc);

    typedef CommCbMemFunT<ConnStateData, CommCloseCbParams> Dialer;
    pinning.closeHandler = JobCallback(33, 5,
                                       Dialer, this, ConnStateData::clientPinnedConnectionClosed);
    // remember the pinned connection so that cb does not unpin a fresher one
    typedef CommCloseCbParams Params;
    Params &params = GetCommParams<Params>(pinning.closeHandler);
    params.conn = pinning.serverConnection;
    comm_add_close_handler(pinning.serverConnection->fd, pinning.closeHandler);
}

/// [re]start monitoring pinned connection for peer closures so that we can
/// propagate them to an _idle_ client pinned to that peer
void
ConnStateData::startPinnedConnectionMonitoring()
{
    if (pinning.readHandler != NULL)
        return; // already monitoring

    typedef CommCbMemFunT<ConnStateData, CommIoCbParams> Dialer;
    pinning.readHandler = JobCallback(33, 3,
                                      Dialer, this, ConnStateData::clientPinnedConnectionRead);
    Comm::Read(pinning.serverConnection, pinning.readHandler);
}

void
ConnStateData::stopPinnedConnectionMonitoring()
{
    if (pinning.readHandler != NULL) {
        Comm::ReadCancel(pinning.serverConnection->fd, pinning.readHandler);
        pinning.readHandler = NULL;
    }
}

/// Our read handler called by Comm when the server either closes an idle pinned connection or
/// perhaps unexpectedly sends something on that idle (from Squid p.o.v.) connection.
void
ConnStateData::clientPinnedConnectionRead(const CommIoCbParams &io)
{
    pinning.readHandler = NULL; // Comm unregisters handlers before calling

    if (io.flag == Comm::ERR_CLOSING)
        return; // close handler will clean up

    // We could use getConcurrentRequestCount(), but this may be faster.
    const bool clientIsIdle = !getCurrentContext();

    debugs(33, 3, "idle pinned " << pinning.serverConnection << " read " <<
           io.size << (clientIsIdle ? " with idle client" : ""));

    assert(pinning.serverConnection == io.conn);
    pinning.serverConnection->close();

    // If we are still sending data to the client, do not close now. When we are done sending,
    // ClientSocketContext::keepaliveNextRequest() checks pinning.serverConnection and will close.
    // However, if we are idle, then we must close to inform the idle client and minimize races.
    if (clientIsIdle && clientConnection != NULL)
        clientConnection->close();
}

const Comm::ConnectionPointer
ConnStateData::validatePinnedConnection(HttpRequest *request, const CachePeer *aPeer)
{
    debugs(33, 7, HERE << pinning.serverConnection);

    bool valid = true;
    if (!Comm::IsConnOpen(pinning.serverConnection))
        valid = false;
    else if (pinning.auth && pinning.host && request && strcasecmp(pinning.host, request->GetHost()) != 0)
        valid = false;
    else if (request && pinning.port != request->port)
        valid = false;
    else if (pinning.peer && !cbdataReferenceValid(pinning.peer))
        valid = false;
    else if (aPeer != pinning.peer)
        valid = false;

    if (!valid) {
        /* The pinning info is not safe, remove any pinning info */
        unpinConnection(true);
    }

    return pinning.serverConnection;
}

Comm::ConnectionPointer
ConnStateData::borrowPinnedConnection(HttpRequest *request, const CachePeer *aPeer)
{
    debugs(33, 7, pinning.serverConnection);
    if (validatePinnedConnection(request, aPeer) != NULL)
        stopPinnedConnectionMonitoring();

    return pinning.serverConnection; // closed if validation failed
}

void
ConnStateData::unpinConnection(const bool andClose)
{
    debugs(33, 3, HERE << pinning.serverConnection);

    if (pinning.peer)
        cbdataReferenceDone(pinning.peer);

    if (Comm::IsConnOpen(pinning.serverConnection)) {
        if (pinning.closeHandler != NULL) {
            comm_remove_close_handler(pinning.serverConnection->fd, pinning.closeHandler);
            pinning.closeHandler = NULL;
        }

        stopPinnedConnectionMonitoring();

        // close the server side socket if requested
        if (andClose)
            pinning.serverConnection->close();
        pinning.serverConnection = NULL;
    }

    safe_free(pinning.host);

    pinning.zeroReply = false;

    /* NOTE: pinning.pinned should be kept. This combined with fd == -1 at the end of a request indicates that the host
     * connection has gone away */
}

