@{
    schema = 1
    project = @{
        name = 'dde'
        type = 'gui'
        'default-target' = 'dde'
    }
    dependencies = @{
        owner = 'application'
    }
    build = @{
        'x64-windows' = @{
            debug = 'debug'
            release = 'release'
        }
    }
    targets = @(
        @{
            id = 'dde'
            kind = 'gui'
            'cmake-target' = 'dde'
            'test-label' = 'dde'
            'debug-path' = 'exe/dde-64d.exe'
            'release-path' = 'exe/dde-64.exe'
            platforms = @('x64-windows')
        }
    )
}
