Str add(var a!N, var b!N) !N {
    ret a + b
    End
}

func update_score(change current!N, var bonus!N) !N {
    current = current + bonus
    ret current
    End
}

func announce() {
    Output.string("Void function called")
    End
}

Str main() {
    change score!N = 100
    var bonus!N = 50
    change total!N = add(score, bonus)
    change updated!N = update_score(score, bonus)
    announce()
    if total == 150 {
        Output.string("Functions working successfully!")
    }
    if updated == 150 {
        Output.string("Mutable parameter working successfully!")
    }
    End
}