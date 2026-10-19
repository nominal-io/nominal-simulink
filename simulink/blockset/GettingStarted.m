%% Getting Started with the Nominal Blockset
% Stream signals from a running Simulink model into a Nominal dataset.
%
% You need an existing dataset in Nominal and its RID, and a profile in Nominal's config
% file holding your API key and your instance's API URL. A computer already set up for
% another Nominal client, such as the Python client or nominal-matlab, has one. Otherwise
% follow API keys setup in the Nominal docs.
%
% 1. In the Library Browser, open *Nominal* and drag *Nominal Stream Sink* into your
%    model.
% 2. Wire the signals you want to record into a Mux, and the Mux into the block.
% 3. Double-click the block and click *Detect from connected signal*.
% 4. Enter the dataset's RID in *Dataset RID*, in quotes.
% 5. Run the model. The block uses the profile named |default|.
%
% Run this file to open the documentation.

nominal_help
