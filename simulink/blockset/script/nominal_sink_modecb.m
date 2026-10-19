function nominal_sink_modecb(blk)
%NOMINAL_SINK_MODECB  Enable only the fields the selected mode actually uses.
%
%   Mask callback for the Nominal Stream Sink's Mode and Create run popups. Direct mode
%   uses a profile, which holds the API key and URL. Proxy mode uses a host and port and
%   needs no credential. Leaving both sets enabled would imply a proxy host matters in
%   direct mode, or that a profile matters in proxy mode. The run's title and ref name
%   matter only when the block creates a run, which it does in direct mode.
%
%   Works by parameter name rather than through the positional MaskEnables array, so
%   adding or reordering a field cannot silently disable the wrong one.
%
%   Called with gcb from the mask; also safe to call directly on a block path.

    m = Simulink.Mask.get(blk);
    if isempty(m)
        return;
    end

    isProxy = strcmp(get_param(blk, 'mode'), 'Via proxy');
    makesRun = ~isProxy && ~strcmp(get_param(blk, 'createRun'), 'None');

    setEnabled(m, 'profile',     ~isProxy);
    setEnabled(m, 'profileFile', ~isProxy);
    setEnabled(m, 'proxyHost',    isProxy);
    setEnabled(m, 'proxyPort',    isProxy);
    setEnabled(m, 'createRun',   ~isProxy);
    setEnabled(m, 'runTitle',     makesRun);
    setEnabled(m, 'refName',      makesRun);
end

function setEnabled(m, name, on)
% Tolerates a missing parameter so an older saved model, or a partially built mask,
% does not error out of the callback and leave the dialog half-configured.
    try
        p = m.getParameter(name);
        if ~isempty(p)
            if on
                p.Enabled = 'on';
            else
                p.Enabled = 'off';
            end
        end
    catch
    end
end
