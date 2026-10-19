function file = package_blockset()
%PACKAGE_BLOCKSET  Build the blockset installer, build/NominalBlockset-<version>.mltbx.
%
%   Packages simulink/blockset as it stands. Everything in that folder ships, apart from
%   the editor and cache files filtered out below, so build it first. `just package`
%   runs the MEX build, the library, the example and the docs before this.
%
%   Needs R2023a or later for ToolboxOptions. The contents it packages are built by the
%   oldest supported release, so the toolbox installs as far back as that release
%   regardless of which one packaged it.
%
%   The platforms the toolbox claims follow the MEX files present: a package built on a
%   machine with only the Windows MEX says so, rather than claiming Linux and failing
%   there at run time. A release is packaged in CI, which builds the Windows, Linux and
%   macOS ones. The oldest release the toolbox claims is the one that saved the library.

    % Fixed for the life of the blockset. It is how an install recognises an earlier
    % version of itself to replace, so changing it would leave users with two copies.
    identifier = '0067b338-49df-45cc-8406-af52de27c0a9';

    simulinkDir = fileparts(fileparts(mfilename('fullpath')));
    root    = fullfile(simulinkDir, 'blockset');
    version = strtrim(fileread(fullfile(simulinkDir, 'VERSION')));
    outDir  = fullfile(fileparts(simulinkDir), 'build');

    % A half-built package installs cleanly and then fails in front of a user, so refuse
    % to make one. The recipe that makes each file is in the message.
    required = {
        fullfile(root, 'library', 'nominal_lib.slx'),                     'just library'
        fullfile(root, 'examples', 'nominal_stream_example.slx'),         'just example'
        fullfile(root, 'doc', 'html', 'nominal_blockset.html'),           'just docs'
        fullfile(root, 'doc', 'html', 'helptoc.xml'),                     'just docs'
    };
    for i = 1:size(required, 1)
        if ~isfile(required{i, 1})
            error('nominal:package', '%s is missing. Run `%s` first.', ...
                  required{i, 1}, required{i, 2});
        end
    end

    hasWin   = isfile(fullfile(root, 'mex', 'nominal_sink_sfun.mexw64'));
    hasLinux = isfile(fullfile(root, 'mex', 'nominal_sink_sfun.mexa64'));
    hasMac   = isfile(fullfile(root, 'mex', 'nominal_sink_sfun.mexmaca64')) || ...
               isfile(fullfile(root, 'mex', 'nominal_sink_sfun.mexmaci64'));
    if ~hasWin && ~hasLinux && ~hasMac
        error('nominal:package', 'no compiled S-function in %s. Run `just build` first.', ...
              fullfile(root, 'mex'));
    end

    % R2021b, except on a Mac, where the library is built in R2023b.
    info    = Simulink.MDLInfo(required{1, 1});
    release = info.ReleaseName;

    writeContents(fullfile(root, 'script', 'Contents.m'), version, release);

    opts = matlab.addons.toolbox.ToolboxOptions(root, identifier);
    opts.ToolboxName    = 'Nominal Blockset';
    opts.ToolboxVersion = version;
    opts.Summary        = 'Stream signals from a running Simulink model into a Nominal dataset.';
    opts.AuthorName     = 'Nominal';
    opts.AuthorCompany  = 'Nominal';
    opts.AuthorEmail    = 'support@nominal.io';

    % Description gave way to Readme in R2026b.
    if isprop(opts, 'Description')
        opts.Description = [ ...
            'A Simulink block that streams its input to a Nominal dataset while the ' ...
            'model runs, either directly over HTTPS or through a relay. If sending ' ...
            'falls behind, the block drops rows rather than slowing the model, and ' ...
            'reports how many.'];
    end

    % Editor backups, Simulink caches and Finder's .DS_Store can land here while working
    % on the blockset, and none of them belong in a release.
    files = opts.ToolboxFiles;
    junk  = endsWith(files, {'.asv', '.autosave', '.slxc', '.mexw64.pdb', '.DS_Store'}) | ...
            contains(files, [filesep 'slprj' filesep]);
    opts.ToolboxFiles = files(~junk);

    opts.ToolboxMatlabPath = fullfile(root, {'library', 'script', 'mex', 'doc'});
    opts.ToolboxGettingStartedGuide = fullfile(root, 'GettingStarted.m');
    opts.MinimumMatlabRelease = release;

    % Field names have changed across releases (Maci64 became Mac), so clear whatever
    % exists rather than naming each.
    platforms = opts.SupportedPlatforms;
    for f = fieldnames(platforms).'
        platforms.(f{1}) = false;
    end
    platforms.Win64   = hasWin;
    platforms.Glnxa64 = hasLinux;
    if isfield(platforms, 'Mac')
        platforms.Mac = hasMac;
    else
        platforms.Maci64 = hasMac;
    end
    opts.SupportedPlatforms = platforms;

    if ~isfolder(outDir)
        mkdir(outDir);
    end
    file = fullfile(outDir, sprintf('NominalBlockset-%s.mltbx', version));
    opts.OutputFile = file;

    matlab.addons.toolbox.packageToolbox(opts);

    fprintf('wrote %s (%s, %s and later)\n', file, strjoin([ ...
        repmat({'Win64'}, 1, hasWin), repmat({'Linux'}, 1, hasLinux), ...
        repmat({'macOS'}, 1, hasMac)], ', '), release);
end

function writeContents(file, version, release)
% Contents.m is what `ver` reads to list an installed toolbox.
    fid = fopen(file, 'w');
    if fid < 0
        error('nominal:package', 'cannot write %s', file);
    end
    closer = onCleanup(@() fclose(fid));
    fprintf(fid, '%% Nominal Blockset\n%% Version %s (%s) %s\n', ...
            version, release, char(datetime('today', 'Format', 'dd-MMM-yyyy')));
end
