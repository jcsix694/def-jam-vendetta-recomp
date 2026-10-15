using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using VendettaSettings.ViewModels;

namespace VendettaSettings.Views;

public partial class ModsView : UserControl
{
    public ModsView()
    {
        InitializeComponent();
        // a mod's zip or folder dropped anywhere on the page installs it
        AddHandler(DragDrop.DropEvent, OnDrop);
        AddHandler(DragDrop.DragOverEvent, OnDragOver);
    }

    private ModsViewModel? Model => DataContext as ModsViewModel;

    private async void OnInstall(object? sender, RoutedEventArgs e)
    {
        if (TopLevel.GetTopLevel(this)?.StorageProvider is not { } storage || Model is not { } model)
            return;
        var files = await storage.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "A mod to install (its .zip)",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("A mod (.zip)") { Patterns = new[] { "*.zip" } },
            },
        });
        if (files.Count > 0 && files[0].TryGetLocalPath() is { } path)
            model.Pick(path);
    }

    private void OnDragOver(object? sender, DragEventArgs e)
    {
        e.DragEffects = e.DataTransfer.Contains(DataFormat.File) ? DragDropEffects.Copy : DragDropEffects.None;
    }

    private void OnDrop(object? sender, DragEventArgs e)
    {
        if (Model is not { } model)
            return;
        var item = e.DataTransfer.TryGetFiles()?.FirstOrDefault();
        if (item?.TryGetLocalPath() is { } path)
            model.Pick(path);
    }
}
