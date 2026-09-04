function nominal_sink_detect(blk)
%NOMINAL_SINK_DETECT  Mask button: fill in channel names from the connected signal.
%
%   Thin wrapper around nominal_sink_autonames so the button reports failures in a
%   dialog rather than as a red stack trace in the console — a mask callback that
%   errors leaves the user staring at an unchanged field with no idea why.
%
%   See also NOMINAL_SINK_AUTONAMES

    try
        nominal_sink_autonames(blk, true);
    catch err
        % Only failures get a dialog. On success the names appear in the field the user
        % is already looking at, so a modal box restating them is just something else to
        % dismiss.
        errordlg(err.message, 'Nominal Stream Sink', 'modal');
    end
end
