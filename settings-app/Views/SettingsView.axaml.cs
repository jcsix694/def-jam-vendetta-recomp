using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using VendettaSettings.ViewModels;

namespace VendettaSettings.Views;

public partial class SettingsView : UserControl
{
    public SettingsView() => InitializeComponent();

    // the custom speed box: shows the speed in use again when left or on Enter
    private void SpeedBoxLostFocus(object? sender, RoutedEventArgs e) => (DataContext as SettingsViewModel)?.TidySpeedText();

    private void SpeedBoxKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter)
            (DataContext as SettingsViewModel)?.TidySpeedText();
    }
}
