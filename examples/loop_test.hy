Str main() {
    change counter!N = 0

    while {
        Output.string("tick")
        counter = counter + 1
        cond-end = (counter < 5, 20)
    }

    if counter == 5 {
        Output.string("done")
    }

    for var index!N = 0 {
        Output.string("for")
        cond-end = (3)
    }
    End
}