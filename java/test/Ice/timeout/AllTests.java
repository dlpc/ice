// **********************************************************************
//
// Copyright (c) 2003-2014 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

package test.Ice.timeout;

import java.io.PrintWriter;

import test.Ice.timeout.Test.TimeoutPrx;
import test.Ice.timeout.Test.TimeoutPrxHelper;

public class AllTests
{
    private static void
    test(boolean b)
    {
        if(!b)
        {
            throw new RuntimeException();
        }
    }

    private static class Callback
    {
        Callback()
        {
            _called = false;
        }

        public synchronized void
        check()
        {
            while(!_called)
            {
                try
                {
                    wait();
                }
                catch(InterruptedException ex)
                {
                }
            }

            _called = false;
        }

        public synchronized void
        called()
        {
            assert(!_called);
            _called = true;
            notify();
        }

        private boolean _called;
    }

    private static class CallbackSuccess extends Ice.Callback
    {
        public void
        completed(Ice.AsyncResult result)
        {
            try
            {
                TimeoutPrx p = TimeoutPrxHelper.uncheckedCast(result.getProxy());
                if(result.getOperation().equals("sendData"))
                {
                    p.end_sendData(result);
                }
                else if(result.getOperation().equals("sleep"))
                {
                    p.end_sleep(result);
                }
            }
            catch(Ice.LocalException ex)
            {
                test(false);
            }
            callback.called();
        }

        public void
        check()
        {
            callback.check();
        }

        private Callback callback = new Callback();
    }

    private static class CallbackFail extends Ice.Callback
    {
        public void
        completed(Ice.AsyncResult result)
        {
            try
            {
                TimeoutPrx p = TimeoutPrxHelper.uncheckedCast(result.getProxy());
                if(result.getOperation().equals("sendData"))
                {
                    p.end_sendData(result);
                }
                else if(result.getOperation().equals("sleep"))
                {
                    p.end_sleep(result);
                }
                test(false);
            }
            catch(Ice.TimeoutException ex)
            {
                callback.called();
            }
            catch(Ice.LocalException ex)
            {
                test(false);
            }
        }

        public void
        check()
        {
            callback.check();
        }

        private Callback callback = new Callback();
    }

    public static TimeoutPrx
    allTests(test.Util.Application app)
    {
        Ice.Communicator communicator = app.communicator();
        PrintWriter out = app.getWriter();

        String sref = "timeout:default -p 12010";
        Ice.ObjectPrx obj = communicator.stringToProxy(sref);
        test(obj != null);

        int mult = 1;
        if(communicator.getProperties().getPropertyWithDefault("Ice.Default.Protocol", "tcp").equals("ssl"))
        {
            mult = 4;
        }

        TimeoutPrx timeout = TimeoutPrxHelper.checkedCast(obj);
        test(timeout != null);

        out.print("testing connect timeout... ");
        out.flush();
        {
            //
            // Expect ConnectTimeoutException.
            //
            TimeoutPrx to = TimeoutPrxHelper.uncheckedCast(obj.ice_timeout(250 * mult));
            to.holdAdapter(500 * mult);
            to.ice_getConnection().close(true); // Force a reconnect.
            try
            {
                to.op();
                test(false);
            }
            catch(Ice.ConnectTimeoutException ex)
            {
                // Expected.
            }
        }
        {
            //
            // Expect success.
            //
            timeout.op(); // Ensure adapter is active.
            TimeoutPrx to = TimeoutPrxHelper.uncheckedCast(obj.ice_timeout(1000 * mult));
            to.holdAdapter(500 * mult);
            to.ice_getConnection().close(true); // Force a reconnect.
            try
            {
                to.op();
            }
            catch(Ice.ConnectTimeoutException ex)
            {
                test(false);
            }
        }
        out.println("ok");

        out.print("testing connection timeout... ");
        out.flush();
        {
            //
            // Expect TimeoutException.
            //
            TimeoutPrx to = TimeoutPrxHelper.uncheckedCast(obj.ice_timeout(250 * mult));
            to.holdAdapter(500);
            try
            {
                to.sendData(new byte[10000000]);
                test(false);
            }
            catch(Ice.TimeoutException ex)
            {
                // Expected.
            }
        }
        {
            //
            // Expect success.
            //
            timeout.op(); // Ensure adapter is active.
            TimeoutPrx to = TimeoutPrxHelper.uncheckedCast(obj.ice_timeout(1000 * mult));
            to.holdAdapter(500);
            try
            {
                to.sendData(new byte[1000000]);
            }
            catch(Ice.TimeoutException ex)
            {
                test(false);
            }
        }
        out.println("ok");

        out.print("testing invocation timeout... ");
        out.flush();
        {
            Ice.Connection connection = obj.ice_getConnection();
            TimeoutPrx to = TimeoutPrxHelper.uncheckedCast(obj.ice_invocationTimeout(250));
            test(connection == to.ice_getConnection());
            try
            {
                to.sleep(500);
                test(false);
            }
            catch(Ice.InvocationTimeoutException ex)
            {
            }
            to = TimeoutPrxHelper.uncheckedCast(obj.ice_invocationTimeout(500));
            test(connection == to.ice_getConnection());
            try
            {
                to.sleep(250);
            }
            catch(Ice.InvocationTimeoutException ex)
            {
                test(false);
            }
            test(connection == to.ice_getConnection());
        }
        {
            //
            // Expect InvocationTimeoutException.
            //
            TimeoutPrx to = TimeoutPrxHelper.uncheckedCast(obj.ice_invocationTimeout(250));
            CallbackFail cb = new CallbackFail();
            to.begin_sleep(500, cb);
            cb.check();
        }
        {
            //
            // Expect success.
            //
            TimeoutPrx to = TimeoutPrxHelper.uncheckedCast(obj.ice_invocationTimeout(500));
            CallbackSuccess cb = new CallbackSuccess();
            to.begin_sleep(250, cb);
            cb.check();
        }
        out.println("ok");

        out.print("testing close timeout... ");
        out.flush();
        {
            TimeoutPrx to = TimeoutPrxHelper.checkedCast(obj.ice_timeout(250));
            Ice.Connection connection = to.ice_getConnection();
            timeout.holdAdapter(750);
            connection.close(false);
            try
            {
                connection.getInfo(); // getInfo() doesn't throw in the closing state.
            }
            catch(Ice.LocalException ex)
            {
                test(false);
            }
            try
            {
                Thread.sleep(500);
            }
            catch(java.lang.InterruptedException ex)
            {
            }
            try
            {
                connection.getInfo();
                test(false);
            }
            catch(Ice.CloseConnectionException ex)
            {
                // Expected.
            }
            timeout.op(); // Ensure adapter is active.
        }
        out.println("ok");

        out.print("testing timeout overrides... ");
        out.flush();
        {
            //
            // Test Ice.Override.Timeout. This property overrides all
            // endpoint timeouts.
            //
            String[] args = new String[0];
            Ice.InitializationData initData = new Ice.InitializationData();
            initData.properties = communicator.getProperties()._clone();
            if(mult == 1)
            {
                initData.properties.setProperty("Ice.Override.Timeout", "250");
            }
            else
            {
                initData.properties.setProperty("Ice.Override.Timeout", "2000");
            }
            Ice.Communicator comm = app.initialize(initData);
            TimeoutPrx to = TimeoutPrxHelper.checkedCast(comm.stringToProxy(sref));
            to.holdAdapter(500);
            try
            {
                to.sendData(new byte[10000000]);
                test(false);
            }
            catch(Ice.TimeoutException ex)
            {
                // Expected.
            }
            //
            // Calling ice_timeout() should have no effect.
            //
            timeout.op(); // Ensure adapter is active.
            to = TimeoutPrxHelper.checkedCast(to.ice_timeout(1000 * mult));
            to.holdAdapter(500);
            try
            {
                to.sendData(new byte[10000000]);
                test(false);
            }
            catch(Ice.TimeoutException ex)
            {
                // Expected.
            }
            comm.destroy();
        }
        {
            //
            // Test Ice.Override.ConnectTimeout.
            //
            String[] args = new String[0];
            Ice.InitializationData initData = new Ice.InitializationData();
            initData.properties = communicator.getProperties()._clone();
            if(mult == 1)
            {
                initData.properties.setProperty("Ice.Override.ConnectTimeout", "250");
            }
            else
            {
                initData.properties.setProperty("Ice.Override.ConnectTimeout", "4000");
            }

            Ice.Communicator comm = app.initialize(initData);
            TimeoutPrx to = TimeoutPrxHelper.uncheckedCast(comm.stringToProxy(sref));
            timeout.holdAdapter(500 * mult);
            try
            {
                to.op();
                test(false);
            }
            catch(Ice.ConnectTimeoutException ex)
            {
                // Expected.
            }
            //
            // Calling ice_timeout() should have no effect on the connect timeout.
            //
            timeout.op(); // Ensure adapter is active.
            timeout.holdAdapter(500 * mult);
            to = TimeoutPrxHelper.uncheckedCast(to.ice_timeout(750 * mult));
            try
            {
                to.op();
                test(false);
            }
            catch(Ice.ConnectTimeoutException ex)
            {
                // Expected.
            }
            //
            // Verify that timeout set via ice_timeout() is still used for requests.
            //
            timeout.op(); // Ensure adapter is active.
            to.op(); // Force connection.
            timeout.holdAdapter(500 * mult);
            to = TimeoutPrxHelper.uncheckedCast(to.ice_timeout(250 * mult));
            try
            {
                to.sendData(new byte[10000000]);
                test(false);
            }
            catch(Ice.TimeoutException ex)
            {
                // Expected.
            }
            comm.destroy();
        }
        {
            //
            // Test Ice.Override.CloseTimeout.
            //
            Ice.InitializationData initData = new Ice.InitializationData();
            initData.properties = communicator.getProperties()._clone();
            initData.properties.setProperty("Ice.Override.CloseTimeout", "200");
            Ice.Communicator comm = app.initialize(initData);
            Ice.Connection connection = comm.stringToProxy(sref).ice_getConnection();
            timeout.holdAdapter(750);
            long now = System.nanoTime();
            comm.destroy();
            test(System.nanoTime() - now < 500 * 1000000);
        }
        out.println("ok");

        out.print("testing invocation timeouts with collocated calls... ");
        out.flush();
        {
            communicator.getProperties().setProperty("TimeoutCollocated.AdapterId", "timeoutAdapter");

            Ice.ObjectAdapter adapter = communicator.createObjectAdapter("TimeoutCollocated");
            adapter.activate();

            TimeoutPrx proxy = TimeoutPrxHelper.uncheckedCast(adapter.addWithUUID(new TimeoutI()));
            proxy = (TimeoutPrx)proxy.ice_invocationTimeout(100);
            try
            {
                proxy.sleep(150);
                test(false);
            }
            catch(Ice.InvocationTimeoutException ex)
            {
            }

            try
            {
                proxy.end_sleep(proxy.begin_sleep(150));
                test(false);
            }
            catch(Ice.InvocationTimeoutException ex)
            {
            }

            ((TimeoutPrx)proxy.ice_invocationTimeout(-1)).ice_ping();

            TimeoutPrx batchTimeout = (TimeoutPrx)proxy.ice_batchOneway();
            batchTimeout.ice_ping();
            batchTimeout.ice_ping();
            batchTimeout.ice_ping();

            ((TimeoutPrx)proxy.ice_invocationTimeout(-1)).begin_sleep(150); // Keep the server thread pool busy.
            try
            {
                batchTimeout.ice_flushBatchRequests();
                test(false);
            }
            catch(Ice.InvocationTimeoutException ex)
            {
            }

            batchTimeout.ice_ping();
            batchTimeout.ice_ping();
            batchTimeout.ice_ping();
            
            ((TimeoutPrx)proxy.ice_invocationTimeout(-1)).begin_sleep(150); // Keep the server thread pool busy.
            try
            {
                batchTimeout.end_ice_flushBatchRequests(batchTimeout.begin_ice_flushBatchRequests());
                test(false);
            }
            catch(Ice.InvocationTimeoutException ex)
            {
            }

            adapter.destroy();
        }
        out.println("ok");

        return timeout;
    }
}
