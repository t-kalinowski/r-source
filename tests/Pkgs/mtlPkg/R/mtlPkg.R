mtl_add <- function(x, y) {
  .Call(mtlPkg_add, x, y)
}

mtl_define_global <- function(x) {
  .Call(mtlPkg_define_global, x)
  invisible(NULL)
}
