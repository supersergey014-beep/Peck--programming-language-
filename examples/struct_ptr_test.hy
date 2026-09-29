pack "point_types.hy"
import "point_helpers.hy"

Str main() {
    change dimensions!N = point_dimension()
    change val!N = 42
    var ptr = &val
    *ptr = 100

    var alias = @val
    ^alias = 101

    var pt_ptr = select(Point)
    pt_ptr.x = val
    pt_ptr.y = 7
    free(pt_ptr)

    Output.string("Structs and pointers operating correctly!")
    End
}