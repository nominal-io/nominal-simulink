classdef SinkBlockTest < BlocksetTestCase
%SINKBLOCKTEST  Offline checks on the sink block. No token, no network.
%
%   Verifies the MEX is on the path, the port sizing follows the channel list, and that a
%   model containing the block actually steps, in both transports and simulation modes.
%   Run via `just sim-test`.
%
%   It cannot check that data lands in Nominal, which needs a token and a real dataset.
%   `just demo <rid>` exercises the write path end to end.

    methods (Test)
        function sfunctionIsOnThePath(tc)
            % Finds the MEX on the path, so a missing build fails here by name rather
            % than inside a model.
            tc.verifyNotEmpty(which('nominal_sink_sfun'));
        end

        function inputWidthFollowsTheChannelList(tc)
            % The port width is derived from the channel list inside mdlInitializeSizes,
            % and if the two ever disagree, nm_push would read past the end of the input
            % signal.
            %
            % No output is expected either. Simulink treats an unconnected output port
            % as a block error, so a sink that always exposed one would force every user
            % to wire up a diagnostic they may not want. showDropped defaults off.
            [model, blk] = SinkBlockTest.directModel('nm_sink_width');

            % CompiledPortWidths only exists while the model is compiled. The
            % model-as-function compile, not SimulationCommand 'update': R2021b's update
            % releases the compiled state before returning, so the query finds nothing.
            feval(model, [], [], [], 'compile');
            term = onCleanup(@() feval(model, [], [], [], 'term'));
            c = get_param(blk, 'CompiledPortWidths');
            delete(term);

            tc.verifyEqual(c.Inport, 3);
            tc.verifyEmpty(c.Outport);
        end

        function stepsWithAnUnreachableEndpoint(~)
            % baseUrl points at a closed port, so the sender thread fails its POSTs.
            % That is the point: a model must keep stepping when the network is
            % unreachable, since telemetry loss is never a reason to stall a simulation.
            model = SinkBlockTest.directModel('nm_sink_steps');
            sim(model);
        end

        function showDroppedAddsAnOutputPort(tc)
            % Turning it on must produce a real port, otherwise the diagnostic is
            % unavailable to the people who do want it.
            [model, blk] = SinkBlockTest.directModel('nm_sink_ports');
            set_param(blk, 'showDropped', 'on');
            add_block('simulink/Sinks/Terminator', [model '/term']);
            add_line(model, 'sink/1', 'term/1');

            % 'Ports' is a static query — [in out enable trigger state ...] — so unlike
            % CompiledPortWidths it needs no compile.
            p = get_param(blk, 'Ports');
            tc.verifyEqual(p(1:2), [1 1]);
        end

        function proxyModeNeedsNoTokenAndNoRelay(tc)
            % Proxy mode must start without a token, because the token stays on the
            % relay. It must also start without a reachable relay, and must not stall
            % the model when none is listening. The port here is closed, so nothing
            % receives the datagrams.
            BlocksetTestCase.setToken(tc, '');
            model = SinkBlockTest.proxyModel('nm_sink_proxy');
            sim(model);
        end

        function doubleQuotedTextWorks(~)
            % "..." is a string, which a C S-function cannot read as text. It used to
            % arrive as empty: a misleading "channel list is empty", and for the URL a
            % silent fallback to the default instance. This test covers the channel
            % list, dataset RID and proxy host, in proxy mode.
            model = BlocksetTestCase.sinkModel('nm_sink_dquote', ...
                'channels',   '"smoke.a, smoke.b, smoke.c"', ...
                'datasetRid', '"ri.catalog.test.dataset.0"', ...
                'mode',       'Via proxy', ...
                'proxyHost',  '"127.0.0.1"', ...
                'proxyPort',  '1');
            SinkBlockTest.feedConstant(model);
            sim(model);
        end

        function aNonTextUrlIsAnError(tc)
            [model, blk] = SinkBlockTest.directModel('nm_sink_badurl');
            set_param(blk, 'baseUrl', '42');
            try
                sim(model);
                tc.verifyFail('a numeric API base URL was accepted. The block should reject it as not text.');
            catch err
                tc.verifySubstring(allMessages(err), 'API base URL must be text');
            end
        end

        function proxyHelloCarriesTheTags(tc)
            % The relay learns a run's tags only from its HELLO frames, so decode what the
            % block actually sends.
            [model, blk] = SinkBlockTest.proxyModel('nm_sink_tagged');
            set_param(blk, 'tags', '["model" "A"; "execution-id" "7"]');
            frames = SinkBlockTest.capture(tc, model, blk);

            hello = frames(strcmp({frames.type}, 'hello'));
            tc.assertNotEmpty(hello, 'the block sent no HELLO.');
            tc.verifyEqual(hello(1).tags, {'model', 'A'; 'execution-id', '7'});
        end

        function zeroTimeStampsFromZero(tc)
            % With t0=0, every run's first row is at 0 and the rest follow at the sample
            % time, exactly, so two runs line up point for point.
            [model, blk] = SinkBlockTest.proxyModel('nm_sink_zero');
            set_param(blk, 'tags', '["model" "A"]', ...
                'timeMode', 'Directly record simulation time (t0=0)');
            frames = SinkBlockTest.capture(tc, model, blk);

            data = frames(strcmp({frames.type}, 'data'));
            tc.assertNotEmpty(data, 'the block sent no rows.');
            times = vertcat(data.timestamps);
            tc.verifyEqual(times, int64((0:numel(times) - 1)' * 1e7));
        end

        function runsInAcceleratorMode(tc)
            % Accelerator mode compiles the model into a MEX of its own, which calls back
            % into the S-function. It needs no Simulink Coder licence, so any user can
            % choose it for a long run, and it should simply work.
            tc.applyFixture(matlab.unittest.fixtures.WorkingFolderFixture);
            model = SinkBlockTest.proxyModel('nm_sink_accel');
            set_param(model, 'SimulationMode', 'accelerator');
            sim(model);
        end
    end

    methods (Static, Access = private)
        function [model, blk] = directModel(name)
            [model, blk] = BlocksetTestCase.sinkModel(name, ...
                'channels',     '''smoke.a, smoke.b, smoke.c''', ...
                'datasetRid',   '''ri.catalog.test.dataset.0''', ...
                'baseUrl',      '''https://127.0.0.1:1''', ...
                'sampleTime',   '0.01', ...
                'capacityRows', '64', ...
                ... % A popup takes its option text here, even though Evaluate is on and
                ... % the S-function receives the 1-based index. Setting '1' is rejected.
                'timeMode',     'Simulation time from start');
            SinkBlockTest.feedConstant(model);
        end

        function [model, blk] = proxyModel(name)
            [model, blk] = BlocksetTestCase.sinkModel(name, ...
                'channels',     '''smoke.a, smoke.b, smoke.c''', ...
                'datasetRid',   '''ri.catalog.test.dataset.0''', ...
                'mode',         'Via proxy', ...
                'proxyHost',    '''127.0.0.1''', ...
                'proxyPort',    '1', ...
                'sampleTime',   '0.01', ...
                'capacityRows', '64');
            SinkBlockTest.feedConstant(model);
        end

        function feedConstant(model)
            add_block('simulink/Sources/Constant', [model '/src'], 'Value', '[1 2 3]');
            add_line(model, 'src/1', 'sink/1');
        end

        function frames = capture(tc, model, blk)
        % Run a proxy-mode model against a socket on this computer and decode every frame
        % it sent. The frames wait in the socket's buffer until the run ends.
            tc.applyFixture(matlab.unittest.fixtures.PathFixture( ...
                fullfile(fileparts(fileparts(fileparts(mfilename('fullpath')))), 'matlab')));
            sock = java.net.DatagramSocket(0, java.net.InetAddress.getByName('127.0.0.1'));
            tc.addTeardown(@() sock.close());
            sock.setSoTimeout(500);

            set_param(blk, 'proxyPort', num2str(sock.getLocalPort()));
            sim(model);

            frames = struct('type', {}, 'tags', {}, 'timestamps', {});
            pkt = java.net.DatagramPacket(zeros(1, 1500, 'int8'), 1500);
            while true
                try
                    sock.receive(pkt);
                catch
                    break;   % timed out: everything the run sent has been read
                end
                bytes = typecast(pkt.getData(), 'uint8');
                f = nmframe_decode(bytes(1:pkt.getLength()));
                frames(end + 1) = struct('type', f.type, ...
                    'tags', {fieldOr(f, 'tags')}, ...
                    'timestamps', fieldOr(f, 'timestamps')); %#ok<AGROW>
            end
        end
    end
end

function v = fieldOr(s, name)
    if isfield(s, name)
        v = s.(name);
    else
        v = [];
    end
end

function s = allMessages(err)
% Simulink wraps the S-function's own message as a cause, so search them all.
    s = err.message;
    for i = 1:numel(err.cause)
        s = [s newline allMessages(err.cause{i})]; %#ok<AGROW>
    end
end
