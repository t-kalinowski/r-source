mtl_add <- function(x, y) {
  .Call(mtlPkg_add, x, y)
}

mtl_define_global <- function(x) {
  .Call(mtlPkg_define_global, x)
  invisible(NULL)
}

mtl_register_callable <- function() {
  invisible(.Call(mtlPkg_register_callable, NULL))
}

mtl_call_callable <- function(x, y) {
  .Call(mtlPkg_call_callable, x, y)
}
