using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using VendettaSettings.ViewModels;

namespace VendettaSettings.Views;

public partial class ProjectDialog : UserControl
{
    public ProjectDialog()
    {
        InitializeComponent();
        // the name box takes the typing as soon as a name dialog opens
        PropertyChanged += (_, e) =>
        {
            if (e.Property == IsVisibleProperty && IsVisible && Model is { IsNameDialog: true })
                Avalonia.Threading.Dispatcher.UIThread.Post(() =>
                {
                    NameBox.Focus();
                    NameBox.SelectAll();
                }, Avalonia.Threading.DispatcherPriority.Background);
        };
    }

    private ProjectsViewModel? Model => DataContext as ProjectsViewModel;

    private async void OnChoosePicture(object? sender, RoutedEventArgs e)
    {
        if (TopLevel.GetTopLevel(this)?.StorageProvider is not { } storage || Model is not { } model)
            return;
        var files = await storage.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "A picture for the project",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("Pictures (.png, .jpg)") { Patterns = new[] { "*.png", "*.jpg", "*.jpeg" } },
            },
        });
        if (files.Count > 0 && files[0].TryGetLocalPath() is { } path)
            model.SetNewPicture(path);
    }
}
