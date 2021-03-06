// Copyright Epic Games, Inc. All Rights Reserved.

//------------------------------------------------------------------------------
// <auto-generated />
//
// This file was automatically generated by SWIG (http://www.swig.org).
// Version 4.0.1
//
// Do not make changes to this file unless you know what you are doing--modify
// the SWIG interface file instead.
//------------------------------------------------------------------------------


public class FDatasmithFacadeLightmassPortal : FDatasmithFacadePointLight {
  private global::System.Runtime.InteropServices.HandleRef swigCPtr;

  internal FDatasmithFacadeLightmassPortal(global::System.IntPtr cPtr, bool cMemoryOwn) : base(DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeLightmassPortal_SWIGUpcast(cPtr), cMemoryOwn) {
    swigCPtr = new global::System.Runtime.InteropServices.HandleRef(this, cPtr);
  }

  internal static global::System.Runtime.InteropServices.HandleRef getCPtr(FDatasmithFacadeLightmassPortal obj) {
    return (obj == null) ? new global::System.Runtime.InteropServices.HandleRef(null, global::System.IntPtr.Zero) : obj.swigCPtr;
  }

  protected override void Dispose(bool disposing) {
    lock(this) {
      if (swigCPtr.Handle != global::System.IntPtr.Zero) {
        if (swigCMemOwn) {
          swigCMemOwn = false;
          DatasmithFacadeCSharpPINVOKE.delete_FDatasmithFacadeLightmassPortal(swigCPtr);
        }
        swigCPtr = new global::System.Runtime.InteropServices.HandleRef(null, global::System.IntPtr.Zero);
      }
      base.Dispose(disposing);
    }
  }

  public FDatasmithFacadeLightmassPortal(string InElementName) : this(DatasmithFacadeCSharpPINVOKE.new_FDatasmithFacadeLightmassPortal(InElementName), true) {
  }

}
