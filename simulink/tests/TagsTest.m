classdef TagsTest < BlocksetTestCase
%TAGSTEST  The Tags field and the t0=0 timestamps that need it. No token, no network.
%
%   Run via `just sim-test`. SinkBlockTest checks that tags and t0=0 timestamps reach the
%   relay in what the block actually sends.

    properties (TestParameter)
        % Every form the field takes, as the text a user types and the tags that result.
        form = struct( ...
            'stringArray', {{'["model" "A"; "execution-id" "7"]', {'model', 'A'; 'execution-id', '7'}}}, ...
            'onePair',     {{'["model" "A"]',                     {'model', 'A'}}}, ...
            'cellArray',   {{'{''model'', ''A''; ''run'', 3}',     {'model', 'A'; 'run', '3'}}}, ...
            'map',         {{'containers.Map({''a''}, {''1''})',  {'a', '1'}}}, ...
            'empty',       {{'[]',                                cell(0, 2)}});

        % Values the field refuses, and the start of the message each gets.
        refused = struct( ...
            'plainText',   {{'"just text"',             'Tags must be pairs of key and value'}}, ...
            'threeColumns',{{'["a" "b" "c"]',           'Tags must be pairs of key and value'}}, ...
            'emptyKey',    {{'["" "v"]',                'a tag key is empty'}}, ...
            'repeatedKey', {{'["k" "1"; "k" "2"]',      'the tag key "k" appears more than once'}}, ...
            'lineBreak',   {{'["k" "a" + newline + "b"]', 'contains a line break'}});
    end

    methods (Test)
        function eachFormGivesItsTags(tc, form)
            tc.verifyEqual(decode(nominal_sink_tags(eval(form{1}))), form{2});
        end

        function badTagsAreRefusedByName(tc, refused)
            tc.verifyError(@() nominal_sink_tags(eval(refused{1})), 'nominal:tags');
            try
                nominal_sink_tags(eval(refused{1}));
            catch err
                tc.verifySubstring(err.message, refused{2});
            end
        end

        function badTagsStopTheRunWithTheirMessage(tc)
            % The field is evaluated when the run starts, and its message has to survive
            % Simulink's wrapping so the user sees what to fix.
            [model, ~] = BlocksetTestCase.sinkModel('nm_tags_bad', ...
                'mode', 'Via proxy', 'proxyPort', '1', ...
                'datasetRid', '''ri.catalog.test.dataset.0''', 'tags', '"just text"');
            add_block('simulink/Sources/Constant', [model '/src'], 'Value', '[1 2 3]');
            add_line(model, 'src/1', 'sink/1');
            try
                sim(model);
                tc.verifyFail('a run with unreadable tags started.');
            catch err
                tc.verifySubstring(allMessages(err), 'Tags must be pairs of key and value');
            end
        end

        function zeroTimeWithoutTagsAsksAndCanBeUndone(tc)
            [~, blk] = BlocksetTestCase.sinkModel('nm_tags_ask');
            set_param(blk, 'timeMode', 'Directly record simulation time (t0=0)');

            asked = containers.Map();
            nominal_sink_timecb(blk, @(q) answer(asked, q, false));
            tc.verifyTrue(asked.isKey('question'), 'choosing t0=0 with no tags did not ask first.');
            tc.verifyEqual(get_param(blk, 'timeMode'), 'Simulation time from start');
        end

        function zeroTimeWithoutTagsCanBeKept(tc)
            [~, blk] = BlocksetTestCase.sinkModel('nm_tags_keep');
            set_param(blk, 'timeMode', 'Directly record simulation time (t0=0)');
            nominal_sink_timecb(blk, @(~) true);
            tc.verifyEqual(get_param(blk, 'timeMode'), 'Directly record simulation time (t0=0)');
        end

        function zeroTimeWithTagsDoesNotAsk(tc)
            % A variable name counts as tags: it may hold them by the time the model runs.
            for tags = {'["model" "A"]', 'nmTags'}
                [~, blk] = BlocksetTestCase.sinkModel(['nm_tags_quiet' num2str(numel(tags{1}))], ...
                    'tags', tags{1});
                set_param(blk, 'timeMode', 'Directly record simulation time (t0=0)');
                asked = containers.Map();
                nominal_sink_timecb(blk, @(q) answer(asked, q, true));
                tc.verifyFalse(asked.isKey('question'), ['asked with tags ' tags{1}]);
            end
        end
    end
end

function keep = answer(record, question, keep)
% Stands in for the dialog: notes the question in a containers.Map, which is a handle,
% and gives the answer the test chose.
    record('question') = question; %#ok<NASGU>
end

function c = decode(encoded)
% Undo nominal_sink_tags' joining, back to an N-by-2 cell array.
    c = cell(0, 2);
    if isempty(encoded)
        return;
    end
    for tag = strsplit(encoded, char(30))
        c(end + 1, :) = strsplit(tag{1}, char(31)); %#ok<AGROW>
    end
end

function s = allMessages(err)
% Simulink wraps the block's own message as a cause, so search them all.
    s = err.message;
    for i = 1:numel(err.cause)
        s = [s newline allMessages(err.cause{i})]; %#ok<AGROW>
    end
end
