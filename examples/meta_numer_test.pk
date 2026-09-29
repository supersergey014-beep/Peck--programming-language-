numer Status {
    Active,
    Disabled
    End
}

change-method lock_var {
    off(edit)
    End
}

Str main() {
    var state = Status.Active

    respon state {
        Status.Active {
            Output.string("Status is active")
        }
        Status.Disabled {
            Output.string("Status is disabled")
        }
        End
    }

    var val!N = 100
    lock_var()
    update()
    update.fn()
    update.str()
    update.other()
    edit()
    edit.fn()
    edit.str()
    edit.other()
    not.edit()
    ret.edit()
    off()
    on()
    next()
    off.next()
    not.requir()
    negative()
    positive()
    input.off()
    Output.string("Meta methods and enums compiled successfully!")
    End
}