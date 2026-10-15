using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using VendettaSettings.ViewModels;

namespace VendettaSettings.Views;

public partial class PlayView : UserControl
{
    public PlayView()
    {
        InitializeComponent();
        // a shared project's zip dropped anywhere on the page imports it
        AddHandler(DragDrop.DropEvent, OnDrop);
        AddHandler(DragDrop.DragOverEvent, OnDragOver);
    }

    private ProjectsViewModel? Model => (DataContext as MainViewModel)?.Projects;

    private async void OnImport(object? sender, RoutedEventArgs e)
    {
        if (TopLevel.GetTopLevel(this)?.StorageProvider is not { } storage || Model is not { } model)
            return;
        var files = await storage.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "A shared project to import (its .zip)",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("A shared project (.zip)") { Patterns = new[] { "*.zip" } },
            },
        });
        if (files.Count > 0 && files[0].TryGetLocalPath() is { } path)
            model.PickImport(path);
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
            model.PickImport(path);
    }
}
