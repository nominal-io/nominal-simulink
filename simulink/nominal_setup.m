function nominal_setup(save)
%NOMINAL_SETUP  Put the Nominal Simulink block on the MATLAB path.
%
%   NOMINAL_SETUP        add the folders for this session
%   NOMINAL_SETUP(true)  add them and save, so they persist across restarts
%
%   Adds this folder, which holds the block library and its mask callbacks, and the
%   blocks/ folder holding the compiled S-function.
%
%   The mask callbacks try to locate this folder themselves, so a block dragged from an
%   open library usually works without running this. Use it when that fails — a model
%   opened on a machine where the library was never opened, or one loaded by a script.
%
%   Example:
%       run('C:\sw\nominal-simulink\simulink\nominal_setup.m')

    if nargin < 1
        save = false;
    end

    here   = fileparts(mfilename('fullpath'));
    blocks = fullfile(here, 'blocks');

    addpath(here);
    if isfolder(blocks)
        addpath(blocks);
    else
        warning('nominal:setup', ...
                ['%s does not exist — the S-function has not been built. ' ...
                 'Run `just build` first.'], blocks);
    end

    if exist(fullfile(here, 'nominal_lib.slx'), 'file') ~= 2
        warning('nominal:setup', ...
                ['nominal_lib.slx is missing — it is generated, not committed. ' ...
                 'Run `just library` to create it.']);
    end

    if save
        savepath;
        fprintf('Added to the MATLAB path and saved:\n  %s\n  %s\n', here, blocks);
    else
        fprintf('Added to the MATLAB path for this session:\n  %s\n  %s\n', here, blocks);
    end
end
