function nominal_sink_timecb(blk, ask)
%NOMINAL_SINK_TIMECB  Confirm t0=0 timestamps on a block that sends no tags.
%
%   Mask callback for the Timestamps popup. With every run stamped from zero, two runs
%   of a model write the same channels at the same times, and without tags nothing in
%   the dataset tells one run's points from another's. The execution ID is a tag too, so
%   this only asks when Tags and Execution ID are both empty. That can still be what
%   someone wants, so this asks rather than refusing. It asks only here, when the option
%   is chosen: a run never stops or warns over it.
%
%   ask is for tests: a function that takes the question and returns true to keep the
%   choice. By default the question goes to a dialog, and with no desktop to show one,
%   the choice is kept.
%
%   Called with gcb from the mask; also safe to call directly on a block path.

    if nargin < 2
        ask = @askInDialog;
    end
    if ~strcmp(get_param(blk, 'timeMode'), 'Directly record simulation time (t0=0)')
        return;
    end

    % Only a field that is plainly empty counts. A variable name might hold tags or an ID
    % by the time the model runs, so it gets the benefit of the doubt.
    empty = '^\s*(''''|""|\[\s*\]|\{\s*\}|strings\(\s*0\s*,\s*2\s*\))?\s*$';
    if isempty(regexp(get_param(blk, 'tags'), empty, 'once')) || ...
       isempty(regexp(get_param(blk, 'executionId'), empty, 'once'))
        return;
    end

    question = ['Without tags or an execution ID, every run of this model writes the same ' ...
                'channels at the same times, so the dataset can''t tell one run''s points ' ...
                'from another''s. Use t0=0 timestamps anyway?'];
    if ~ask(question)
        set_param(blk, 'timeMode', 'Simulation time from start');
    end
end

function keep = askInDialog(question)
    if ~usejava('desktop')
        keep = true;
        return;
    end
    answer = questdlg(question, 'Nominal Stream Sink', 'Use t0=0', 'Cancel', 'Cancel');
    keep = strcmp(answer, 'Use t0=0');
end
