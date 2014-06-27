// **********************************************************************
//
// Copyright (c) 2003-2014 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#include <Ice/Outgoing.h>
#include <Ice/Object.h>
#include <Ice/CollocatedRequestHandler.h>
#include <Ice/ConnectionI.h>
#include <Ice/Reference.h>
#include <Ice/Endpoint.h>
#include <Ice/LocalException.h>
#include <Ice/Protocol.h>
#include <Ice/Instance.h>
#include <Ice/ReplyStatus.h>
#include <Ice/ProxyFactory.h>

using namespace std;
using namespace Ice;
using namespace Ice::Instrumentation;
using namespace IceInternal;

IceInternal::Outgoing::Outgoing(IceProxy::Ice::Object* proxy, const string& operation, OperationMode mode, 
                                const Context* context) :
    _proxy(proxy),
    _mode(mode),
    _observer(proxy, operation, context),
    _state(StateUnsent),
    _encoding(getCompatibleEncoding(proxy->__reference()->getEncoding())),
    _is(proxy->__reference()->getInstance().get(), Ice::currentProtocolEncoding),
    _os(proxy->__reference()->getInstance().get(), Ice::currentProtocolEncoding),
    _sent(false)
{ 
    checkSupportedProtocol(getCompatibleProtocol(proxy->__reference()->getProtocol()));

    switch(_proxy->__reference()->getMode())
    {
        case Reference::ModeTwoway:
        case Reference::ModeOneway:
        case Reference::ModeDatagram:
        {
            _os.writeBlob(requestHdr, sizeof(requestHdr));
            break;
        }

        case Reference::ModeBatchOneway:
        case Reference::ModeBatchDatagram:
        {
            while(true)
            {
                try
                {
                    _handler = proxy->__getRequestHandler(true);
                    _handler->prepareBatchRequest(&_os);
                    break;
                }
                catch(const RetryException&)
                {
                    _proxy->__setRequestHandler(_handler, 0); // Clear request handler and retry.
                }
                catch(const Ice::LocalException& ex)
                {
                    _observer.failed(ex.ice_name());
                    _proxy->__setRequestHandler(_handler, 0); // Clear request handler
                    throw;
                }
            }
            break;
        }
    }

    try
    {
        _os.write(_proxy->__reference()->getIdentity());

        //
        // For compatibility with the old FacetPath.
        //
        if(_proxy->__reference()->getFacet().empty())
        {
            _os.write(static_cast<string*>(0), static_cast<string*>(0));
        }
        else
        {
            string facet = _proxy->__reference()->getFacet();
            _os.write(&facet, &facet + 1);
        }

        _os.write(operation, false);

        _os.write(static_cast<Byte>(mode));

        if(context != 0)
        {
            //
            // Explicit context
            //
            _os.write(*context);
        }
        else
        {
            //
            // Implicit context
            //
            const ImplicitContextIPtr& implicitContext = _proxy->__reference()->getInstance()->getImplicitContext();
            const Context& prxContext = _proxy->__reference()->getContext()->getValue();
            if(implicitContext == 0)
            {
                _os.write(prxContext);
            }
            else
            {
                implicitContext->write(prxContext, &_os);
            }
        }
    }
    catch(const LocalException& ex)
    {
        abort(ex);
    }
}

Outgoing::~Outgoing()
{
}

bool
IceInternal::Outgoing::invoke()
{
    assert(_state == StateUnsent);
    
    const Reference::Mode mode = _proxy->__reference()->getMode();
    if(mode == Reference::ModeBatchOneway || mode == Reference::ModeBatchDatagram)
    {
        _state = StateInProgress;
        _handler->finishBatchRequest(&_os);
        return true;
    }

    int cnt = 0;
    while(true)
    {        
        try
        {
            _state = StateInProgress;
            _exception.reset(0);
            _sent = false;

            _handler = _proxy->__getRequestHandler(false);

            if(_handler->sendRequest(this)) // Request sent and no response expected, we're done.
            {
                return true;
            }
                    
            bool timedOut = false;
            {
                IceUtil::Monitor<IceUtil::Mutex>::Lock sync(_monitor);
                        
                //
                // If the handler says it's not finished, we wait until we're done.
                //
                int invocationTimeout = _proxy->__reference()->getInvocationTimeout();
                if(invocationTimeout > 0)
                {
                    IceUtil::Time now = IceUtil::Time::now();
                    IceUtil::Time deadline = now + IceUtil::Time::milliSeconds(invocationTimeout);
                    while((_state == StateInProgress || !_sent) && _state != StateFailed && !timedOut)
                    {
                        _monitor.timedWait(deadline - now);
                            
                        if((_state == StateInProgress || !_sent) && _state != StateFailed)
                        {
                            now = IceUtil::Time::now();
                            timedOut = now >= deadline;
                        }
                    }
                }
                else
                {
                    while((_state == StateInProgress || !_sent) && _state != StateFailed)
                    {
                        _monitor.wait();
                    }
                }
            }
                
            if(timedOut)
            {
                _handler->requestTimedOut(this);

                //
                // Wait for the exception to propagate. It's possible the request handler ignores
                // the timeout if there was a failure shortly before requestTimedOut got called. 
                // In this case, the exception should be set on the Outgoing.
                //
                IceUtil::Monitor<IceUtil::Mutex>::Lock sync(_monitor);
                while(!_exception.get())
                {
                    _monitor.wait();
                }
            }
                
            if(_exception.get())
            {
                _exception->ice_throw();
            }
            else
            {
                assert(_state != StateInProgress);
                return _state == StateOK;
            }
        }
        catch(const RetryException&)
        {
            _proxy->__setRequestHandler(_handler, 0); // Clear request handler and retry.
        }
        catch(const Ice::Exception& ex)
        {
            try
            {
                int interval = _proxy->__handleException(ex, _handler, _mode, _sent, cnt);
                _observer.retried(); // Invocation is being retried.
                if(interval > 0)
                {
                    IceUtil::ThreadControl::sleep(IceUtil::Time::milliSeconds(interval));
                }
            }
            catch(const Ice::Exception& ex)
            {
                _observer.failed(ex.ice_name());
                throw;
            }
        }
    }

    assert(false);
    return false;
}

void
IceInternal::Outgoing::abort(const LocalException& ex)
{
    assert(_state == StateUnsent);
    
    //
    // If we didn't finish a batch oneway or datagram request, we must
    // notify the connection about that we give up ownership of the
    // batch stream.
    //
    if(_proxy->__reference()->getMode() == Reference::ModeBatchOneway || 
       _proxy->__reference()->getMode() == Reference::ModeBatchDatagram)
    {
        _handler->abortBatchRequest();
    }
    
    ex.ice_throw();
}

bool
IceInternal::Outgoing::send(const Ice::ConnectionIPtr& connection, bool compress, bool response)
{
    return connection->sendRequest(this, compress, response);
}

void
IceInternal::Outgoing::invokeCollocated(CollocatedRequestHandler* handler)
{
    handler->invokeRequest(this);
}

void
IceInternal::Outgoing::sent()
{
    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(_monitor);
    if(_proxy->__reference()->getMode() != Reference::ModeTwoway)
    {
        _remoteObserver.detach();
        _state = StateOK;
    }
    _sent = true;
    _monitor.notify();

    //
    // NOTE: At this point the stack allocated Outgoing object can be destroyed 
    // since the notify() on the monitor will release the thread waiting on the
    // synchronous Ice call.
    //
}

void
IceInternal::Outgoing::finished(const Exception& ex, bool sent)
{
    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(_monitor);
    assert(_state <= StateInProgress);
    _remoteObserver.failed(ex.ice_name());
    _remoteObserver.detach();

    _state = StateFailed;
    _exception.reset(ex.ice_clone());
    _sent = sent;
    _monitor.notify();
}

void
IceInternal::Outgoing::finished(BasicStream& is)
{
    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(_monitor);

    assert(_proxy->__reference()->getMode() == Reference::ModeTwoway); // Can only be called for twoways.

    assert(_state <= StateInProgress);
    if(_remoteObserver)
    {
        _remoteObserver->reply(static_cast<Int>(is.b.size() - headerSize - 4));
    }
    _remoteObserver.detach();

    _is.swap(is);

    Byte replyStatus;
    _is.read(replyStatus);
    
    switch(replyStatus)
    {
        case replyOK:
        {
            _state = StateOK; // The state must be set last, in case there is an exception.
            break;
        }
        
        case replyUserException:
        {
            _observer.userException();
            _state = StateUserException; // The state must be set last, in case there is an exception.
            break;
        }
        
        case replyObjectNotExist:
        case replyFacetNotExist:
        case replyOperationNotExist:
        {
            //
            // Don't read the exception members directly into the
            // exception. Otherwise if reading fails and raises an
            // exception, you will have a memory leak.
            //
            Identity ident;
            _is.read(ident);

            //
            // For compatibility with the old FacetPath.
            //
            vector<string> facetPath;
            _is.read(facetPath);
            string facet;
            if(!facetPath.empty())
            {
                if(facetPath.size() > 1)
                {
                    throw MarshalException(__FILE__, __LINE__);
                }
                facet.swap(facetPath[0]);
            }

            string operation;
            _is.read(operation, false);
            
            RequestFailedException* ex;
            switch(replyStatus)
            {
                case replyObjectNotExist:
                {
                    ex = new ObjectNotExistException(__FILE__, __LINE__);
                    break;
                }
                
                case replyFacetNotExist:
                {
                    ex = new FacetNotExistException(__FILE__, __LINE__);
                    break;
                }
                
                case replyOperationNotExist:
                {
                    ex = new OperationNotExistException(__FILE__, __LINE__);
                    break;
                }
                
                default:
                {
                    ex = 0; // To keep the compiler from complaining.
                    assert(false);
                    break;
                }
            }
            
            ex->id = ident;
            ex->facet = facet;
            ex->operation = operation;
            _exception.reset(ex);

            _state = StateLocalException; // The state must be set last, in case there is an exception.
            break;
        }
        
        case replyUnknownException:
        case replyUnknownLocalException:
        case replyUnknownUserException:
        {
            //
            // Don't read the exception members directly into the
            // exception. Otherwise if reading fails and raises an
            // exception, you will have a memory leak.
            //
            string unknown;
            _is.read(unknown, false);
            
            UnknownException* ex;
            switch(replyStatus)
            {
                case replyUnknownException:
                {
                    ex = new UnknownException(__FILE__, __LINE__);
                    break;
                }
                
                case replyUnknownLocalException:
                {
                    ex = new UnknownLocalException(__FILE__, __LINE__);
                    break;
                }
                
                case replyUnknownUserException:
                {
                    ex = new UnknownUserException(__FILE__, __LINE__);
                    break;
                }
                
                default:
                {
                    ex = 0; // To keep the compiler from complaining.
                    assert(false);
                    break;
                }
            }
            
            ex->unknown = unknown;
            _exception.reset(ex);

            _state = StateLocalException; // The state must be set last, in case there is an exception.
            break;
        }
        
        default:
        {
            _exception.reset(new UnknownReplyStatusException(__FILE__, __LINE__));
            _state = StateLocalException;
            break;
        }
    }

    _monitor.notify();
}

void
IceInternal::Outgoing::throwUserException()
{
    try
    {
        _is.startReadEncaps();
        _is.throwException();
    }
    catch(const Ice::UserException&)
    {
        _is.endReadEncaps();
        throw;
    }
}

IceInternal::BatchOutgoing::BatchOutgoing(IceProxy::Ice::Object* proxy, const string& name) :
    _proxy(proxy),
    _connection(0),
    _sent(false),
    _os(proxy->__reference()->getInstance().get(), Ice::currentProtocolEncoding),
    _observer(proxy, name, 0)
{
    checkSupportedProtocol(proxy->__reference()->getProtocol());
}

IceInternal::BatchOutgoing::BatchOutgoing(ConnectionI* connection, Instance* instance, const string& name) :
    _proxy(0),
    _connection(connection),
    _sent(false), 
    _os(instance, Ice::currentProtocolEncoding),
    _observer(instance, name)
{
}

void
IceInternal::BatchOutgoing::invoke()
{
    assert(_proxy || _connection);

    if(_connection)
    {
        if(_connection->flushBatchRequests(this))
        {
            return;
        }

        IceUtil::Monitor<IceUtil::Mutex>::Lock sync(_monitor);
        while(!_exception.get() && !_sent)
        {
            _monitor.wait();
        }
        if(_exception.get())
        {
            _exception->ice_throw();
        }
        return;
    }

    RequestHandlerPtr handler;
    try
    {
        handler = _proxy->__getRequestHandler(false);
        if(handler->sendRequest(this))
        {
            return;
        }

        bool timedOut = false;
        {
            IceUtil::Monitor<IceUtil::Mutex>::Lock sync(_monitor);
            int timeout = _proxy->__reference()->getInvocationTimeout();
            if(timeout > 0)
            {
                IceUtil::Time now = IceUtil::Time::now();
                IceUtil::Time deadline = now + IceUtil::Time::milliSeconds(timeout);
                while(!_exception.get() && !_sent && !timedOut)
                {
                    _monitor.timedWait(deadline - now);                
                    if(!_exception.get() && !_sent)
                    {
                        now = IceUtil::Time::now();
                        timedOut = now >= deadline;
                    }
                }
            }
            else
            {
                while(!_exception.get() && !_sent)
                {
                    _monitor.wait();
                }
            }
        }

        if(timedOut)
        {
            handler->requestTimedOut(this);

            //
            // Wait for the exception to propagate. It's possible the request handler ignores
            // the timeout if there was a failure shortly before requestTimedOut got called. 
            // In this case, the exception should be set on the Outgoing.
            //
            IceUtil::Monitor<IceUtil::Mutex>::Lock sync(_monitor);
            while(!_exception.get())
            {
                _monitor.wait();
            }
        }
    
        if(_exception.get())
        {
            _exception->ice_throw();
        }
    }
    catch(const RetryException&)
    {
        //
        // Clear request handler but don't retry or throw. Retrying
        // isn't useful, there were no batch requests associated with
        // the proxy's request handler.
        //
        _proxy->__setRequestHandler(handler, 0); 
    }
    catch(const Ice::Exception& ex)
    {
        _proxy->__setRequestHandler(handler, 0); // Clear request handler
        _observer.failed(ex.ice_name());
        throw; // Throw to notify the user that batch requests were potentially lost.
    }
}

bool
IceInternal::BatchOutgoing::send(const Ice::ConnectionIPtr& connection, bool, bool)
{
    return connection->flushBatchRequests(this);
}

void
IceInternal::BatchOutgoing::invokeCollocated(CollocatedRequestHandler* handler)
{
    handler->invokeBatchRequests(this);
}

void
IceInternal::BatchOutgoing::sent()
{
    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(_monitor);
    _remoteObserver.detach();
    
    _sent = true;
    _monitor.notify();

    //
    // NOTE: At this point the stack allocated BatchOutgoing object
    // can be destroyed since the notify() on the monitor will release
    // the thread waiting on the synchronous Ice call.
    //
}

void
IceInternal::BatchOutgoing::finished(const Ice::Exception& ex, bool)
{
    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(_monitor);
    _remoteObserver.failed(ex.ice_name());
    _remoteObserver.detach();
    _exception.reset(ex.ice_clone());
    _monitor.notify();
}
