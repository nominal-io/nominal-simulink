classdef InstalledToolboxTest < matlab.unittest.TestCase
%INSTALLEDTOOLBOXTEST  The packaged blockset works once installed, with nothing else set up.
%
%   Every other test runs the blockset from the clone, with its folders put on the path
%   by hand. A user has none of that: they install the .mltbx and expect the block to be
%   in the Library Browser and to run. This installs the package built by
%   package_blockset, checks exactly that, and uninstalls it again.
%
%   Run it in a fresh MATLAB session with only this folder on the path, as
%   `just install-test` does. With the clone on the path it would test the clone.
%
%   It refuses to run if the blockset is already installed, rather than replace and then
%   remove someone's working copy.

    properties (Constant)
        % Must match package_blockset.
        Identifier = '0067b338-49df-45cc-8406-af52de27c0a9';
    end

    properties
        Version
        InstallFolder
    end

    methods (TestClassSetup)
        function install(tc)
            tc.fatalAssertEmpty(which('nominal_sink_sfun'), ...
                ['The blockset is already on the path, so this would test that copy ' ...
                 'rather than the package. Remove the blockset folders from the path ' ...
                 '(nominal_setup(true) saves them) and run this test in a fresh ' ...
                 'MATLAB session.']);

            installed = matlab.addons.installedAddons;
            tc.fatalAssertFalse(any(strcmp(installed.Identifier, tc.Identifier)), ...
                'The Nominal Blockset is already installed. Uninstall it first.');

            simulinkDir = fileparts(fileparts(mfilename('fullpath')));
            tc.Version  = strtrim(fileread(fullfile(simulinkDir, 'VERSION')));
            file = fullfile(fileparts(simulinkDir), 'build', ...
                            sprintf('NominalBlockset-%s.mltbx', tc.Version));
            tc.fatalAssertTrue(isfile(file), sprintf('%s is missing. Run `just package`.', file));

            matlab.addons.install(file);
            tc.addTeardown(@() matlab.addons.uninstall(tc.Identifier));
            % Teardowns run last-first, so this runs before the uninstall. Windows will
            % not delete a loaded DLL, and a MEX still loaded leaves the folder behind.
            tc.addTeardown(@() releaseBlockset());

            tc.InstallFolder = fileparts(fileparts(which('nominal_root')));
        end
    end

    methods (TestMethodTeardown)
        function closeModels(~)
            bdclose('all');
        end
    end

    methods (Test)
        function blocksetComesFromTheInstall(tc)
            tc.assertNotEmpty(tc.InstallFolder, 'nominal_root is not on the path after installing');
            clone = fileparts(fileparts(fileparts(mfilename('fullpath'))));
            tc.verifyFalse(startsWith(tc.InstallFolder, clone), ...
                sprintf('the blockset resolved to the clone (%s), not the install', tc.InstallFolder));
            for f = {'nominal_sink_sfun', 'nominal_sink_detect', 'nominal_help'}
                tc.verifyTrue(startsWith(which(f{1}), tc.InstallFolder), ...
                    sprintf('%s does not resolve into the installed blockset', f{1}));
            end
            % Each library in the Library Browser has its own slblocks, and the Browser
            % reads all of them, so ours only has to be among them rather than first.
            tc.verifyTrue(any(startsWith(which('slblocks', '-all'), tc.InstallFolder)), ...
                'the installed slblocks is not on the path');
        end

        function installedVersionMatches(tc)
            installed = matlab.addons.installedAddons;
            row = installed(strcmp(installed.Identifier, tc.Identifier), :);
            tc.verifyEqual(char(row.Version), tc.Version);
        end

        function libraryIsListedInTheLibraryBrowser(tc)
            % Both halves: slblocks names the library, and the library was saved with
            % the repository flag on. Either missing and the Browser shows nothing.
            %
            % Run from library/ so `slblocks` resolves to ours, because the current
            % folder takes precedence over the path.
            back = cd(fullfile(tc.InstallFolder, 'library'));
            restore = onCleanup(@() cd(back));
            s = slblocks;
            delete(restore);
            tc.verifyEqual(s.Browser.Library, 'nominal_lib');
            tc.verifyEqual(s.Browser.Name, 'Nominal');
            load_system('nominal_lib');
            tc.verifyEqual(get_param('nominal_lib', 'EnableLBRepository'), 'on');
        end

        function helpPageIsInstalled(tc)
            % Help is not clicked here, since that opens a browser. Check the hook names
            % nominal_help and that the page it would open exists.
            load_system('nominal_lib');
            h = get_param('nominal_lib/Nominal Stream Sink', 'MaskHelp');
            tc.verifySubstring(h, 'nominal_help(gcb)');
            tc.verifyTrue(isfile(fullfile(tc.InstallFolder, 'doc', 'html', ...
                                          'nominal_stream_sink.html')));
        end

        function blockRunsInANewModel(tc)
            model = 'nm_installed_test';
            new_system(model);
            add_block('nominal_lib/Nominal Stream Sink', [model '/sink']);
            set_param([model '/sink'], ...
                'channels', '''a, b, c''', ...
                'datasetRid', '''ri.catalog.test.dataset.0''', ...
                'mode', 'Via proxy', 'proxyHost', '''127.0.0.1''', 'proxyPort', '1', ...
                'sampleTime', '0.01', 'capacityRows', '64');
            add_block('simulink/Sources/Constant', [model '/src'], 'Value', '[1 2 3]');
            add_line(model, 'src/1', 'sink/1');
            set_param(model, 'StopTime', '0.05', 'SolverType', 'Fixed-step', ...
                             'FixedStep', '0.01');
            sim(model);
        end

        function exampleRuns(tc)
            example = fullfile(tc.InstallFolder, 'examples', 'nominal_stream_example.slx');
            tc.assertTrue(isfile(example));
            load_system(example);
            sim('nominal_stream_example', 'StopTime', '0.1');
        end
    end
end

function releaseBlockset()
    bdclose('all');
    clear mex %#ok<CLMEX>
end
