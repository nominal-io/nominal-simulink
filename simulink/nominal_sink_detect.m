function nominal_sink_detect(blk)
%NOMINAL_SINK_DETECT  Mask button: fill in channel names from the connected signal.
%
%   Thin wrapper around nominal_sink_autonames so the button reports failures in a
%   dialog rather than as a red stack trace in the console — a mask callback that
%   errors leaves the user staring at an unchanged field with no idea why.
%
%   See also NOMINAL_SINK_AUTONAMES

    try
        names = nominal_sink_autonames(blk, true);
    catch err
        errordlg(err.message, 'Nominal Stream Sink', 'modal');
        return;
    end

    % Say what happened. The count is the useful part: it has to match the width of the
    % signal being recorded, and seeing "4 channels" when you expected 3 is the whole
    % point of showing it.
    msgbox(sprintf('Detected %d channel(s):\n\n%s', numel(names), ...
                   strjoin(names, sprintf('\n'))), ...
           'Nominal Stream Sink', 'modal');
end
