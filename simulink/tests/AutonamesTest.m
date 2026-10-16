classdef AutonamesTest < BlocksetTestCase
%AUTONAMESTEST  Channel-name detection across the shapes people actually wire up.
%
%   The button itself cannot be clicked in -batch, so this exercises the function behind
%   it. Needs no token and no network.

    methods (Test)
        function muxOfNamedLines(tc)
            [~, blk] = AutonamesTest.buildMux('nm_names_mux', true);
            tc.verifyEqual(nominal_sink_autonames(blk), {'alpha', 'beta', 'gamma'});
        end

        function muxOfUnnamedLinesUsesBlockNames(tc)
            % Worse than naming the signals but far better than failing.
            [~, blk] = AutonamesTest.buildMux('nm_names_unnamed', false);
            tc.verifyEqual(nominal_sink_autonames(blk), {'s1', 's2', 's3'});
        end

        function busCreatorUsesItsElementNames(tc)
            [model, blk] = BlocksetTestCase.sinkModel('nm_names_bus');
            add_block('simulink/Signal Routing/Bus Creator', [model '/bus'], 'Inputs', '3');
            AutonamesTest.feed(model, 'bus', {'rpm', 'egt', 'psi'});
            add_line(model, 'bus/1', 'sink/1');
            tc.verifyEqual(nominal_sink_autonames(blk), {'rpm', 'egt', 'psi'});
        end

        function duplicateNamesAreMadeUnique(tc)
            % Two signals sharing a name would collapse into one channel in Nominal and
            % lose data with no error, so they must be made distinct.
            [model, blk] = BlocksetTestCase.sinkModel('nm_names_dup');
            add_block('simulink/Signal Routing/Mux', [model '/mux'], 'Inputs', '2');
            AutonamesTest.feed(model, 'mux', {'dup', 'dup'});
            add_line(model, 'mux/1', 'sink/1');
            tc.verifyEqual(nominal_sink_autonames(blk), {'dup', 'dup_2'});
        end

        function aSuffixNeverCollidesWithAnotherName(tc)
            % Suffixing the second 'x' as 'x_2' would collide with the third signal,
            % which is already called that.
            [model, blk] = BlocksetTestCase.sinkModel('nm_names_suffix');
            add_block('simulink/Signal Routing/Mux', [model '/mux'], 'Inputs', '3');
            AutonamesTest.feed(model, 'mux', {'x', 'x', 'x_2'});
            add_line(model, 'mux/1', 'sink/1');
            names = nominal_sink_autonames(blk);
            tc.verifyEqual(numel(unique(names)), 3, strjoin(names, ', '));
            tc.verifyEqual(names([1 3]), {'x', 'x_2'}, 'names the user gave are kept');
        end

        function busSelectorPortsNameOnlyTheirOwnSignal(tc)
            % With "Output as virtual bus" off, each selected signal leaves on its own
            % port. Naming the whole selection made the channel list wider than the
            % input, and the model failed to compile.
            [model, blk] = AutonamesTest.buildSelector('nm_names_selector', 'off');
            tc.verifyEqual(nominal_sink_autonames(blk), {'c'});
            set_param(blk, 'channels', '''c''');
            sim(model, 'StopTime', '0.03');
        end

        function busSelectorBusNamesTheWholeSelection(tc)
            [model, blk] = AutonamesTest.buildSelector('nm_names_selbus', 'on');
            tc.verifyEqual(nominal_sink_autonames(blk), {'a', 'c'});
            set_param(blk, 'channels', '''a, c''');
            sim(model, 'StopTime', '0.03');
        end

        function anUnconnectedInputIsReported(tc)
            [~, blk] = BlocksetTestCase.sinkModel('nm_names_none');
            tc.verifyError(@() nominal_sink_autonames(blk), 'nominal:autonames');
        end

        function theMaskCallbackWorksOffThePath(tc)
            % The failure a user actually hits. The mask callbacks live in the blockset's
            % script/ folder, and dragging a block into a model does not put that folder
            % on the MATLAB path. Every test above passes because the harness put it
            % there, which is precisely why they missed this: pressing the button in a
            % real model gave "Undefined function 'nominal_sink_detect'".
            %
            % So remove the folder from the path, then evaluate the callback exactly as
            % Simulink would.
            [~, blk] = AutonamesTest.buildMux('nm_names_offpath', true);
            AutonamesTest.leavePath(tc, {'script'});
            tc.assertEmpty(which('nominal_sink_detect'), ...
                'could not remove script/ from the path, so this test would mean nothing');

            % Straight from the dialog control. MaskCallbackString would need splitting
            % on '|', which mangles the '||' in the path-fix expression — and the button
            % is a dialog control, so it is not in there anyway.
            src = Simulink.Mask.get(blk).getDialogControl('detectNames').Callback;
            AutonamesTest.evalCallback(blk, src);

            tc.verifyEqual(nominal_sink_autonames(blk), {'alpha', 'beta', 'gamma'});
        end

        function aModelRunsOffThePath(tc)
            % The failure this reproduces — "S-Function 'nominal_sink_sfun' does not
            % exist" — appears only when the model is *run*, so
            % theMaskCallbackWorksOffThePath does not catch it. A dragged-in block
            % resolves its dialog fine and then fails at compile.
            [model, blk] = AutonamesTest.buildMux('nm_names_runoff', true);
            set_param(blk, 'channels', '''alpha, beta, gamma''', ...
                           'datasetRid', '''ri.catalog.test.dataset.0''', ...
                           'baseUrl', '''https://127.0.0.1:1''', ...
                           'sampleTime', '0.01', 'capacityRows', '64');
            AutonamesTest.leavePath(tc, {'script', 'mex'});
            tc.assertEmpty(which('nominal_sink_sfun'), ...
                'could not hide the S-function, so this test would mean nothing');

            sim(model, 'StopTime', '0.03');
        end
    end

    methods (Static, Access = private)
        function [model, blk] = buildMux(name, named)
            [model, blk] = BlocksetTestCase.sinkModel(name);
            add_block('simulink/Signal Routing/Mux', [model '/mux'], 'Inputs', '3');
            labels = {'alpha', 'beta', 'gamma'};
            if ~named
                labels = {'', '', ''};
            end
            AutonamesTest.feed(model, 'mux', labels);
            add_line(model, 'mux/1', 'sink/1');
        end

        function [model, blk] = buildSelector(name, asBus)
        % a, b, c into a Bus Creator; a Bus Selector picks a and c. With asBus off it
        % has a port per signal, and port 2 (carrying c) is the one wired to the sink.
            [model, blk] = BlocksetTestCase.sinkModel(name, ...
                'datasetRid', '''ri.catalog.test.dataset.0''', 'mode', 'Via proxy', ...
                'proxyHost', '''127.0.0.1''', 'proxyPort', '1');
            add_block('simulink/Signal Routing/Bus Creator', [model '/bus'], 'Inputs', '3');
            AutonamesTest.feed(model, 'bus', {'a', 'b', 'c'});
            add_block('simulink/Signal Routing/Bus Selector', [model '/sel']);
            add_line(model, 'bus/1', 'sel/1');
            set_param([model '/sel'], 'OutputSignals', 'a,c', 'OutputAsBus', asBus);
            if strcmp(asBus, 'on')
                add_line(model, 'sel/1', 'sink/1');
            else
                add_line(model, 'sel/2', 'sink/1');
                add_block('simulink/Sinks/Terminator', [model '/term']);
                add_line(model, 'sel/1', 'term/1');
            end
        end

        function feed(model, into, labels)
        % One Constant per label, named s1, s2, ..., each line named by its label.
            for i = 1:numel(labels)
                add_block('simulink/Sources/Constant', sprintf('%s/s%d', model, i), ...
                          'Value', num2str(i));
                h = add_line(model, sprintf('s%d/1', i), sprintf('%s/%d', into, i));
                if ~isempty(labels{i})
                    set_param(h, 'Name', labels{i});
                end
            end
        end

        function leavePath(tc, folders)
        % Take blockset folders off the path for the rest of the test.
        %
        % Leaving the current folder matters as much as rmpath: MATLAB resolves
        % functions in the current folder whatever the path says, so running the suite
        % from inside one of these folders would hide the very failure being tested.
            tc.applyFixture(matlab.unittest.fixtures.CurrentFolderFixture(tempdir));
            for f = folders
                dir = BlocksetTestCase.folder(f{1});
                rmpath(dir);
                tc.addTeardown(@addpath, dir);
            end
        end

        function evalCallback(blk, src)
        % Simulink evaluates callbacks with gcb set to the block, so reproduce that.
            set_param(bdroot(blk), 'CurrentBlock', get_param(blk, 'Name'));
            evalin('base', src);
        end
    end
end
