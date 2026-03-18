function Component()
{
}

Component.prototype.createOperations = function()
{
    component.createOperations();

    if (systemInfo.productType === "windows") {
        // Start Menu shortcut
        component.addOperation("CreateShortcut",
            "@TargetDir@/talk-qt.exe",
            "@StartMenuDir@/TalQ.lnk",
            "workingDirectory=@TargetDir@",
            "iconPath=@TargetDir@/talk-qt.exe",
            "description=Nextcloud Talk Desktop Client");

        // Desktop shortcut
        component.addOperation("CreateShortcut",
            "@TargetDir@/talk-qt.exe",
            "@DesktopDir@/TalQ.lnk",
            "workingDirectory=@TargetDir@",
            "iconPath=@TargetDir@/talk-qt.exe",
            "description=Nextcloud Talk Desktop Client");
    }
}
