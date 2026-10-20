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
            % The test profile's URL is a closed port, so the sender thread fails its POSTs.
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

        function proxyModeNeedsNoProfileAndNoRelay(~)
            % Proxy mode must start without a profile, because the API key stays on the
            % relay. It must also start without a reachable relay, and must not stall
            % the model when none is listening. The port here is closed, so nothing
            % receives the datagrams.
            [model, blk] = SinkBlockTest.proxyModel('nm_sink_proxy');
            set_param(blk, 'profileFile', '''no-such-folder/config.yml''');
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

        function aNonTextProfileIsAnError(tc)
            [model, blk] = SinkBlockTest.directModel('nm_sink_badprofile');
            set_param(blk, 'profile', '42');
            tc.verifySubstring(SinkBlockTest.runError(model), 'Profile must be text');
        end

        function profileProblemsStopTheRunByName(tc)
            % Each way a profile can be missing gets its own message, naming the file, so
            % the fix is plain from the error alone.
            [model, blk] = SinkBlockTest.directModel('nm_sink_noprofile');
            set_param(blk, 'profileFile', '''no-such-folder/config.yml''');
            tc.verifySubstring(SinkBlockTest.runError(model), ...
                'could not read the Nominal config file no-such-folder/config.yml');

            set_param(blk, 'profileFile', ['''' BlocksetTestCase.profileFile() ''''], ...
                'profile', '''staging''');
            tc.verifySubstring(SinkBlockTest.runError(model), 'has no profile named ''staging''');
        end

        function proxyHelloCarriesTheTags(tc)
            % The relay learns a run's tags only from its HELLO frames, so decode what the
            % block actually sends. The execution ID comes last.
            [model, blk] = SinkBlockTest.proxyModel('nm_sink_tagged');
            set_param(blk, 'tags', '["model" "A"; "parameter-set" "34"]', 'executionId', '7');
            frames = SinkBlockTest.capture(tc, blk, @() sim(model));

            hello = frames(strcmp({frames.type}, 'hello'));
            tc.assertNotEmpty(hello, 'the block sent no HELLO.');
            tc.verifyEqual(hello(1).tags, ...
                {'model', 'A'; 'parameter-set', '34'; 'execution-id', '7'});
        end

        function autoExecutionIdsDifferBetweenRuns(tc)
            % The default 'auto' makes a new ID each run, which is what keeps runs of one
            % model apart with no setup.
            [model, blk] = SinkBlockTest.proxyModel('nm_sink_autoid');
            frames = SinkBlockTest.capture(tc, blk, @() [sim(model), sim(model)]);

            ids = SinkBlockTest.executionIds(frames);
            tc.verifyNumElements(ids, 2, 'two runs did not get two execution IDs.');
            tc.verifyTrue(all(~cellfun(@isempty, ...
                regexp(ids, '^\d{8}-\d{6}-[0-9a-f]{6}$', 'once'))), ...
                ['an automatic ID is not the UTC start time and six hex digits: ' strjoin(ids, ', ')]);
        end

        function aSimulationInputLoopSetsEachRun(tc)
            % The documented way to run a campaign: the fields name variables, and each
            % run's SimulationInput sets them.
            [model, blk] = SinkBlockTest.proxyModel('nm_sink_campaign');
            set_param(blk, 'tags', 'nmTags', 'executionId', 'execId', ...
                'timeMode', 'Directly record simulation time (t0=0)');
            params = [34 77 34];
            in = repmat(Simulink.SimulationInput(model), 1, 3);
            for k = 1:3
                in(k) = in(k).setVariable('execId', k);
                in(k) = in(k).setVariable('nmTags', ["parameter-set" string(params(k))]);
            end
            frames = SinkBlockTest.capture(tc, blk, @() sim(in, 'ShowProgress', 'off'));

            hello = frames(strcmp({frames.type}, 'hello'));
            seen = containers.Map();
            for h = hello
                seen(h.tags{end, 2}) = h.tags{1, 2};
            end
            tc.verifyEqual(sort(seen.keys), {'1', '2', '3'});
            tc.verifyEqual(cellfun(@(k) seen(k), {'1', '2', '3'}, 'UniformOutput', false), ...
                {'34', '77', '34'});
        end

        function zeroTimeStampsFromZero(tc)
            % With t0=0, every run's first row is at 0 and the rest follow at the sample
            % time, exactly, so two runs line up point for point.
            [model, blk] = SinkBlockTest.proxyModel('nm_sink_zero');
            set_param(blk, 'tags', '["model" "A"]', ...
                'timeMode', 'Directly record simulation time (t0=0)');
            frames = SinkBlockTest.capture(tc, blk, @() sim(model));

            data = frames(strcmp({frames.type}, 'data'));
            tc.assertNotEmpty(data, 'the block sent no rows.');
            times = vertcat(data.timestamps);
            tc.verifyEqual(times, int64((0:numel(times) - 1)' * 1e7));
        end

        function theMaximumDelayReachesTheStream(tc)
            % The field is seconds in the dialog and milliseconds in the library, and a
            % short delay still streams every row.
            [model, blk] = SinkBlockTest.proxyModel('nm_sink_delay');
            set_param(blk, 'maxDelay', '0.02');
            frames = SinkBlockTest.capture(tc, blk, @() sim(model));
            data = frames(strcmp({frames.type}, 'data'));
            tc.assertNotEmpty(data, 'the block sent no rows.');
            tc.verifyNumElements(vertcat(data.timestamps), 6);
        end

        function numberFieldsRefuseValuesThatAreNotPositive(tc)
            % No number field has a value that stands for its default. 0 or less used to
            % be replaced quietly, so -1 in Sample time meant 1 ms, and text in quotes
            % was read as the code of its first character.
            cases = { ...
                'sampleTime',   '0',      'Sample time (s) must be a number greater than 0'; ...
                'sampleTime',   '-1',     'Sample time (s) must be a number greater than 0'; ...
                'capacityRows', '0',      'Buffer capacity (rows) must be a whole number greater than 0'; ...
                'capacityRows', '64.5',   'Buffer capacity (rows) must be a whole number greater than 0'; ...
                'maxDelay',     '0',      'Maximum delay before sending (s) must be a number greater than 0'; ...
                'maxDelay',     '''1''',  'Maximum delay before sending (s) must be a number greater than 0'};
            for k = 1:size(cases, 1)
                [model, blk] = SinkBlockTest.proxyModel(sprintf('nm_sink_number%d', k));
                text = '';
                % The error comes when the value is applied, which set_param does.
                try
                    set_param(blk, cases{k, 1}, cases{k, 2});
                    sim(model);
                catch err
                    text = allMessages(err);
                end
                tc.verifySubstring(text, cases{k, 3}, ...
                    sprintf('%s = %s was not refused.', cases{k, 1}, cases{k, 2}));
                bdclose(model);
            end
        end

        function aRunThatCantBeCreatedWarnsAndTheModelRuns(tc)
            % The test profile's URL is a closed port, so creating the run fails. That
            % must not stop the model, and the warning names the title the block made.
            [model, blk] = SinkBlockTest.directModel('nm_sink_run');
            set_param(blk, 'createRun', 'One per execution', 'executionId', '7');
            out = simOutput(model);
            tc.verifySubstring(out, ...
                'could not create the run ''nm_sink_run, execution 7''');
            tc.verifySubstring(out, 'The rows still go to the dataset.');
        end

        function aRunPerModelIsTitledByTheModel(tc)
            [model, blk] = SinkBlockTest.directModel('nm_sink_permodel');
            set_param(blk, 'createRun', 'One per model');
            tc.verifySubstring(simOutput(model), ...
                'could not create the run ''nm_sink_permodel''');
        end

        function aRunTitleNamesEachExecutionsRun(tc)
            % A run per execution needs a title of its own, since the relay finds an
            % execution's run by its title after a restart, so Run title replaces only
            % the model's name and the execution ID still follows.
            [model, blk] = SinkBlockTest.directModel('nm_sink_titled');
            set_param(blk, 'createRun', 'One per execution', 'runTitle', '"Bench test"', ...
                'executionId', '7');
            tc.verifySubstring(simOutput(model), ...
                'could not create the run ''Bench test, execution 7''');
        end

        function aRunPerModelUsesTheTitleAsGiven(tc)
            [model, blk] = SinkBlockTest.directModel('nm_sink_titledmodel');
            set_param(blk, 'createRun', 'One per model', 'runTitle', '"Bench test"');
            tc.verifySubstring(simOutput(model), 'could not create the run ''Bench test''');
        end

        function anEmptyRefNameStopsTheRun(tc)
            [model, blk] = SinkBlockTest.directModel('nm_sink_noref');
            set_param(blk, 'createRun', 'One per execution', 'refName', '''''');
            tc.verifySubstring(SinkBlockTest.runError(model), 'Ref name is empty');
        end

        function proxyModeAsksTheRelayForTheRun(tc)
            % In proxy mode the relay makes the run, so the HELLO has to carry all of it,
            % and END has to say when the execution's rows end. END goes three times, since
            % UDP can lose any one.
            [model, blk] = SinkBlockTest.proxyModel('nm_sink_proxyrun');
            set_param(blk, 'createRun', 'One per execution', 'executionId', '7', ...
                'timeMode', 'Directly record simulation time (t0=0)');
            frames = SinkBlockTest.capture(tc, blk, @() sim(model));

            hello = frames(strcmp({frames.type}, 'hello'));
            tc.assertNotEmpty(hello, 'the block sent no HELLO.');
            tc.verifyEqual(hello(1).run, struct('mode', 'own', 'start', int64(0), ...
                'tagCount', 1, 'title', 'nm_sink_proxyrun, execution 7', 'refName', 'default'));

            data = frames(strcmp({frames.type}, 'data'));
            ends = frames(strcmp({frames.type}, 'end'));
            tc.assertNotEmpty(data, 'the block sent no rows.');
            tc.verifyNumElements(ends, 3);
            tc.verifyEqual([ends.endTime], repmat(max(vertcat(data.timestamps)), 1, numel(ends)));
        end

        function aProxyRunPerModelShowsTheWholeDataset(tc)
            % A run per model holds every execution, so it matches no tags. Matching the
            % first execution's would leave out every later one whose tags differ.
            [model, blk] = SinkBlockTest.proxyModel('nm_sink_proxymodel');
            set_param(blk, 'createRun', 'One per model', 'tags', '["model" "A"]');
            frames = SinkBlockTest.capture(tc, blk, @() sim(model));

            hello = frames(strcmp({frames.type}, 'hello'));
            tc.assertNotEmpty(hello, 'the block sent no HELLO.');
            tc.verifyEqual({hello(1).run.mode, hello(1).run.tagCount, hello(1).run.title}, ...
                {'shared', 0, 'nm_sink_proxymodel'});
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

        function frames = capture(tc, blk, run)
        % Point a proxy-mode block at a socket on this computer, call run, and decode
        % every frame the block sent. The frames wait in the socket's buffer meanwhile.
            tc.applyFixture(matlab.unittest.fixtures.PathFixture( ...
                fullfile(fileparts(fileparts(fileparts(mfilename('fullpath')))), 'matlab')));
            sock = java.net.DatagramSocket(0, java.net.InetAddress.getByName('127.0.0.1'));
            tc.addTeardown(@() sock.close());
            sock.setSoTimeout(500);

            set_param(blk, 'proxyPort', num2str(sock.getLocalPort()));
            run();

            frames = struct('type', {}, 'tags', {}, 'timestamps', {}, 'run', {}, 'endTime', {});
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
                    'timestamps', fieldOr(f, 'timestamps'), ...
                    'run', fieldOr(f, 'run'), ...
                    'endTime', fieldOr(f, 'endTime')); %#ok<AGROW>
            end
        end

        function text = runError(model)
        % Run the model, expecting it to stop, and return every message in the error.
            text = '';
            try
                sim(model);
            catch err
                text = allMessages(err);
            end
        end

        function ids = executionIds(frames)
        % The distinct execution-id values the HELLOs announced, in the order first seen.
            ids = {};
            for f = frames(strcmp({frames.type}, 'hello'))
                row = strcmp(f.tags(:, 1), 'execution-id');
                if any(row) && ~any(strcmp(ids, f.tags{row, 2}))
                    ids{end + 1} = f.tags{row, 2}; %#ok<AGROW>
                end
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

function out = simOutput(model) %#ok<INUSD> used in evalc
% What sim prints, on one line. MATLAB wraps a warning to the width of the window it
% prints to, so a phrase can break across lines in one session and not another.
    out = regexprep(evalc('sim(model)'), '\s+', ' ');
end

function s = allMessages(err)
% Simulink wraps the S-function's own message as a cause, so search them all.
    s = err.message;
    for i = 1:numel(err.cause)
        s = [s newline allMessages(err.cause{i})]; %#ok<AGROW>
    end
end
