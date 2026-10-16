classdef SinkBlockTest < BlocksetTestCase
%SINKBLOCKTEST  Offline checks on the sink block. No token, no network.
%
%   Verifies the MEX loads, the port sizing follows the channel list, and that a model
%   containing the block actually steps, in both transports and simulation modes. Run
%   via `just sim-test`.
%
%   The one thing it cannot check is that data lands in Nominal — that needs a token,
%   and cstream's nmstream-demo already covers the write path end to end.

    methods (Test)
        function sfunctionIsOnThePath(tc)
            % A statically linked MEX still fails to load if a dependency is missing,
            % and the error is unhelpful, so check this first and separately.
            tc.verifyNotEmpty(which('nominal_sink_sfun'));
        end

        function inputWidthFollowsTheChannelList(tc)
            % Worth asserting: the port width is derived from the channel list inside
            % mdlInitializeSizes, and if the two ever disagree, nm_push would read past
            % the end of the input signal.
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
            % Pointed at a closed port, so nothing receives the datagrams. Proxy mode
            % must not need a token, since the whole point is that the token stays on
            % the relay; must not need a reachable relay to start; and must not stall
            % the model when the relay is absent.
            BlocksetTestCase.setToken(tc, '');
            model = SinkBlockTest.proxyModel('nm_sink_proxy');
            sim(model);
        end

        function doubleQuotedTextWorks(~)
            % "..." is a string, which a C S-function cannot read as text. It used to
            % arrive as empty: a misleading "channel list is empty", and for the URL a
            % silent fall back to the default instance.
            model = BlocksetTestCase.sinkModel('nm_sink_dquote', ...
                'channels',   '"smoke.a, smoke.b, smoke.c"', ...
                'datasetRid', '"ri.catalog.test.dataset.0"', ...
                'mode',       'Via proxy', ...
                'proxyHost',  '"127.0.0.1"', ...
                'proxyPort',  '1');
            SinkBlockTest.feedConstant(model);
            sim(model);
        end

        function aNonTextUrlIsAnErrorNotTheDefault(tc)
            [model, blk] = SinkBlockTest.directModel('nm_sink_badurl');
            set_param(blk, 'baseUrl', '42');
            try
                sim(model);
                tc.verifyFail('a numeric API base URL ran, so it fell back to a default');
            catch err
                tc.verifySubstring(allMessages(err), 'API base URL must be text');
            end
        end

        function runsInAcceleratorMode(tc)
            % Accelerator mode compiles the model into a MEX of its own, which calls back
            % into the S-function. Users reach for it on long runs, and it needs no
            % Simulink Coder licence, so it should simply work.
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
    end
end

function s = allMessages(err)
% Simulink wraps the S-function's own message as a cause, so search them all.
    s = err.message;
    for i = 1:numel(err.cause)
        s = [s newline allMessages(err.cause{i})]; %#ok<AGROW>
    end
end
