function nominal_setup(save)
%NOMINAL_SETUP  Use the Nominal blockset straight from a clone of this repository.
%
%   NOMINAL_SETUP        add the blockset's folders for this session
%   NOMINAL_SETUP(true)  add them and save, so they persist across restarts
%
%   Installing the toolbox (.mltbx) sets up the path itself. Use nominal_setup when
%   working on the blockset from a clone. It adds the folders that installation would:
%   library/ (the block library and slblocks.m, which lists it in the Library Browser),
%   script/ (the mask callbacks), mex/ (the compiled S-function) and doc/ (the
%   documentation).
%
%   The mask callbacks try to locate these folders themselves, so a block dragged from an
%   open library works without running this. Use it when that fails, for example for a
%   model opened in a MATLAB session where nominal_lib is not loaded, or a model loaded
%   by a script.
%
%   Example:
%       run(fullfile('<clone>', 'simulink', 'nominal_setup.m'))

    if nargin < 1
        save = false;
    end

    root    = fullfile(fileparts(mfilename('fullpath')), 'blockset');
    folders = fullfile(root, {'library', 'script', 'mex', 'doc'});

    addpath(folders{:});

    % What a fresh clone is missing, and the recipe that makes each.
    expected = {
        fullfile(root, 'mex', ['nominal_sink_sfun.' mexext]), 'just build'
        fullfile(root, 'library', 'nominal_lib.slx'),         'just library'
        fullfile(root, 'doc', 'html', 'nominal_blockset.html'), 'just docs'
    };
    for i = 1:size(expected, 1)
        if ~isfile(expected{i, 1})
            warning('nominal:setup', '%s is missing — it is generated, not committed. Run `%s`.', ...
                    expected{i, 1}, expected{i, 2});
        end
    end

    if save
        savepath;
        fprintf('Added to the MATLAB path and saved:\n');
    else
        fprintf('Added to the MATLAB path for this session:\n');
    end
    fprintf('  %s\n', folders{:});
end
