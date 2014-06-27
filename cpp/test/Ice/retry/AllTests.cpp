// **********************************************************************
//
// Copyright (c) 2003-2014 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#include <Ice/Ice.h>
#include <TestCommon.h>
#include <Test.h>
#include <InstrumentationI.h>
#include <SystemFailure.h>

using namespace std;
using namespace Test;

class CallbackBase : public IceUtil::Monitor<IceUtil::Mutex>
{
public:

    CallbackBase() :
        _called(false)
    {
    }

    virtual ~CallbackBase()
    {
    }

    void check()
    {
        IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);
        while(!_called)
        {
            wait();
        }
        _called = false;
    }

protected:

    void called()
    {
        IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);
        assert(!_called);
        _called = true;
        notify();
    }

private:

    bool _called;
};

class CallbackSuccess : public IceUtil::Shared, public CallbackBase
{
public:

    void response()
    {
        called();
    }

    void exception(const ::Ice::Exception&)
    {
        test(false);
    }
};
typedef IceUtil::Handle<CallbackSuccess> CallbackSuccessPtr;

class CallbackFail : public IceUtil::Shared, public CallbackBase
{
public:

    void response()
    {
        test(false);
    }

    void exception(const ::Ice::Exception& ex)
    {
        test(dynamic_cast<const Ice::ConnectionLostException*>(&ex) ||
             dynamic_cast<const Ice::UnknownLocalException*>(&ex));
        called();
    }
};
typedef IceUtil::Handle<CallbackFail> CallbackFailPtr;

RetryPrx
allTests(const Ice::CommunicatorPtr& communicator)
{
    cout << "testing stringToProxy... " << flush;
    string ref = "retry:default -p 12010";
    Ice::ObjectPrx base1 = communicator->stringToProxy(ref);
    test(base1);
    Ice::ObjectPrx base2 = communicator->stringToProxy(ref);
    test(base2);
    cout << "ok" << endl;

    cout << "testing checked cast... " << flush;
    RetryPrx retry1 = RetryPrx::checkedCast(base1);
    test(retry1);
    test(retry1 == base1);
    RetryPrx retry2 = RetryPrx::checkedCast(base2);
    test(retry2);
    test(retry2 == base2);
    cout << "ok" << endl;

    cout << "calling regular operation with first proxy... " << flush;
    retry1->op(false);
    cout << "ok" << endl;

    int invocationCount = 3;

    cout << "calling operation to kill connection with second proxy... " << flush;
    try
    {
        retry2->op(true);
        test(false);
    }
    catch(const Ice::UnknownLocalException&)
    {
        // Expected with collocation
    }
    catch(const Ice::ConnectionLostException&)
    {
    }
    testInvocationCount(invocationCount + 1);
    testFailureCount(1);
    testRetryCount(0);
    cout << "ok" << endl;

    cout << "calling regular operation with first proxy again... " << flush;
    retry1->op(false);
    testInvocationCount(invocationCount + 2);
    testFailureCount(1);
    testRetryCount(0);
    cout << "ok" << endl;

    CallbackSuccessPtr cb1 = new CallbackSuccess();
    CallbackFailPtr cb2 = new CallbackFail();

    cout << "calling regular AMI operation with first proxy... " << flush;
    retry1->begin_op(false, newCallback_Retry_op(cb1, &CallbackSuccess::response, &CallbackSuccess::exception));
    cb1->check();
    testInvocationCount(invocationCount + 3);
    testFailureCount(1);
    testRetryCount(0);
    cout << "ok" << endl;

    cout << "calling AMI operation to kill connection with second proxy... " << flush;
    retry2->begin_op(true, newCallback_Retry_op(cb2, &CallbackFail::response, &CallbackFail::exception));
    cb2->check();
    testInvocationCount(invocationCount + 4);
    testFailureCount(2);
    testRetryCount(0);
    cout << "ok" << endl;

    cout << "calling regular AMI operation with first proxy again... " << flush;
    retry1->begin_op(false, newCallback_Retry_op(cb1, &CallbackSuccess::response, &CallbackSuccess::exception));
    cb1->check();
    testInvocationCount(invocationCount + 5);
    testFailureCount(2);
    testRetryCount(0);
    cout << "ok" << endl;
    
    cout << "testing idempotent operation... " << flush;
    test(retry1->opIdempotent(0) == 4);
    testInvocationCount(invocationCount + 6);
    testFailureCount(2);
    testRetryCount(4);
    test(retry1->end_opIdempotent(retry1->begin_opIdempotent(4)) == 8);
    testInvocationCount(invocationCount + 7);
    testFailureCount(2);
    testRetryCount(8);
    cout << "ok" << endl;

    cout << "testing non-idempotent operation... " << flush;
    try
    {
        retry1->opNotIdempotent(8);
        test(false);
    }
    catch(const Ice::LocalException&)
    {
    }
    testInvocationCount(invocationCount + 8);
    testFailureCount(3);
    testRetryCount(8);
    try
    {
        retry1->end_opNotIdempotent(retry1->begin_opNotIdempotent(9));
        test(false);
    }
    catch(const Ice::LocalException&)
    {
    }
    testInvocationCount(invocationCount + 9);
    testFailureCount(4);
    testRetryCount(8);
    cout << "ok" << endl;

    if(!retry1->ice_getConnection())
    {
        invocationCount = invocationCount + 10;
        cout << "testing system exception... " << flush;
        try
        {
            retry1->opSystemException();
            test(false);
        }
        catch(const SystemFailure&)
        {
        }
        test(invocationCount + 1);
        testFailureCount(5);
        testRetryCount(8);
        try
        {
            retry1->end_opSystemException(retry1->begin_opSystemException());
            test(false);
        }
        catch(const SystemFailure&)
        {
        }
        testInvocationCount(invocationCount + 2);
        testFailureCount(6);
        testRetryCount(8);
        cout << "ok" << endl;
    }

    return retry1;
}
